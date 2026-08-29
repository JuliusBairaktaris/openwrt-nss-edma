// SPDX-License-Identifier: GPL-2.0-only
/* Host driver for the NSS firmware behind the Wi-Fi data plane.
 *
 * This is the half that prepares a core: its clocks and supply are turned on,
 * the instruction memory it boots with is cleared, and the firmware image is
 * copied into the reserved region it runs from. The core is left in reset;
 * releasing it belongs with the shared memory the firmware expects to find
 * already described, which is not here yet.
 *
 * None of it happens on probe. A running core takes over the switch CPU port,
 * so an image that started one while probing could not also bring up the host
 * ethernet stack; preparation is asked for through debugfs instead, and a
 * reboot returns the board to the host data plane.
 */

#include <linux/cleanup.h>
#include <linux/clk.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>

struct nss_clk_cfg {
	const char *name;
	unsigned long rate;
};

/* Every clock the core needs, at the rate the firmware is built for. The
 * fabric clocks are shared between the two cores; a Wi-Fi data plane boots
 * only core 0, so they are taken from its node along with its own. The core
 * clock alone carries no rate here: the device tree states the frequencies
 * this SoC supports, and the core runs at the highest of them.
 */
static const struct nss_clk_cfg nss_clks[] = {
	{ "nss-core-clk",			0 },
	{ "nss-noc-clk",		461500000 },
	{ "nss-ptp-ref-clk",		150000000 },
	{ "nss-csr-clk",		200000000 },
	{ "nss-cfg-clk",		100000000 },
	{ "nss-imem-clk",		400000000 },
	{ "nss-nssnoc-qosgen-ref-clk",	 19200000 },
	{ "nss-mem-noc-nss-axi-clk",	461500000 },
	{ "nss-nssnoc-snoc-clk",	266600000 },
	{ "nss-nssnoc-timeout-ref-clk",	  4800000 },
	{ "nss-ce-axi-clk",		200000000 },
	{ "nss-ce-apb-clk",		200000000 },
	{ "nss-nssnoc-ce-axi-clk",	200000000 },
	{ "nss-nssnoc-ce-apb-clk",	200000000 },
	{ "nss-nssnoc-ahb-clk",		200000000 },
	{ "nss-ahb-clk",		200000000 },
	{ "nss-axi-clk",		461500000 },
	{ "nss-mpt-clk",		 25000000 },
	{ "nss-nc-axi-clk",		461500000 },
};

struct nss_core {
	struct device *dev;
	void __iomem *imem;
	resource_size_t imem_size;
	phys_addr_t load_addr;
	resource_size_t region_base;
	resource_size_t region_size;
	unsigned long core_rate;
	u32 id;
	bool loaded;
	struct mutex lock;
	struct dentry *debugfs;
	struct clk_bulk_data clks[];
};

static void nss_clocks_disable(void *data)
{
	struct nss_core *core = data;

	clk_bulk_disable_unprepare(ARRAY_SIZE(nss_clks), core->clks);
}

static int nss_clocks_get(struct nss_core *core)
{
	int i, ret;

	for (i = 0; i < ARRAY_SIZE(nss_clks); i++)
		core->clks[i].id = nss_clks[i].name;

	ret = devm_clk_bulk_get(core->dev, ARRAY_SIZE(nss_clks), core->clks);
	if (ret)
		return dev_err_probe(core->dev, ret, "clocks\n");

	for (i = 0; i < ARRAY_SIZE(nss_clks); i++) {
		unsigned long rate = nss_clks[i].rate ? : core->core_rate;

		ret = clk_set_rate(core->clks[i].clk, rate);
		if (ret)
			return dev_err_probe(core->dev, ret, "%s rate\n",
					     nss_clks[i].name);
	}

	return 0;
}

static int nss_clocks_enable(struct nss_core *core)
{
	int ret;

	ret = clk_bulk_prepare_enable(ARRAY_SIZE(nss_clks), core->clks);
	if (ret)
		return dev_err_probe(core->dev, ret, "clock enable\n");

	return devm_add_action_or_reset(core->dev, nss_clocks_disable, core);
}

/* The firmware is linked to run from a fixed address, so it is copied to
 * that address rather than anywhere within the region. The region the
 * device tree reserved is what bounds the copy: an image larger than the
 * space between its load address and the end of the region would run into
 * whatever follows it.
 */
static int nss_firmware_load(struct nss_core *core)
{
	resource_size_t room = core->region_base + core->region_size -
			       core->load_addr;
	const struct firmware *fw;
	void __iomem *dst;
	char name[32];
	int ret;

	snprintf(name, sizeof(name), "qca-nss%u-retail.bin", core->id);

	ret = request_firmware(&fw, name, core->dev);
	if (ret)
		return dev_err_probe(core->dev, ret, "no %s\n", name);

	if (fw->size > room) {
		dev_err(core->dev, "%s is %zu bytes, %pa of room\n", name,
			fw->size, &room);
		ret = -EFBIG;
		goto out;
	}

	dst = ioremap(core->load_addr, fw->size);
	if (!dst) {
		ret = -ENOMEM;
		goto out;
	}

	memcpy_toio(dst, fw->data, fw->size);
	iounmap(dst);

	dev_info(core->dev, "loaded %s, %zu bytes at %pa\n", name, fw->size,
		 &core->load_addr);
out:
	release_firmware(fw);

	return ret;
}

static int nss_core_prepare(struct nss_core *core)
{
	int ret;

	guard(mutex)(&core->lock);

	if (core->loaded)
		return -EBUSY;

	ret = nss_clocks_enable(core);
	if (ret)
		return ret;

	/* The core executes out of its instruction memory and reads what it
	 * finds there, whatever a previous occupant left behind. The window is
	 * cleared through an uncached mapping, so what the core reads is what
	 * was written with no cache maintenance to get right.
	 */
	memset_io(core->imem, 0, core->imem_size);

	ret = nss_firmware_load(core);
	if (ret)
		return ret;

	core->loaded = true;

	return 0;
}

static int nss_load_set(void *data, u64 val)
{
	if (val != 1)
		return -EINVAL;

	return nss_core_prepare(data);
}

static int nss_load_get(void *data, u64 *val)
{
	struct nss_core *core = data;

	guard(mutex)(&core->lock);
	*val = core->loaded;

	return 0;
}

DEFINE_DEBUGFS_ATTRIBUTE(nss_load_fops, nss_load_get, nss_load_set, "%llu\n");

/* The region the core boots from is described once, on the node the two
 * cores share, and each core's load address points into it.
 */
static int nss_region_get(struct nss_core *core)
{
	struct device_node *cmn __free(device_node) =
		of_find_compatible_node(NULL, NULL, "qcom,nss-common");
	struct device_node *mem;
	struct reserved_mem *rmem;

	if (!cmn)
		return -ENODEV;

	mem = of_parse_phandle(cmn, "memory-region", 0);
	if (!mem)
		return -ENODEV;

	rmem = of_reserved_mem_lookup(mem);
	of_node_put(mem);
	if (!rmem)
		return -ENODEV;

	core->region_base = rmem->base;
	core->region_size = rmem->size;

	return 0;
}

static int nss_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct nss_core *core;
	u32 load_addr, rate;
	struct resource *res;
	int ret;

	core = devm_kzalloc(dev, struct_size(core, clks, ARRAY_SIZE(nss_clks)),
			    GFP_KERNEL);
	if (!core)
		return -ENOMEM;

	core->dev = dev;

	ret = devm_mutex_init(dev, &core->lock);
	if (ret)
		return ret;

	ret = of_property_read_u32(dev->of_node, "qcom,id", &core->id);
	if (ret)
		return dev_err_probe(dev, ret, "no qcom,id\n");

	ret = of_property_read_u32(dev->of_node, "qcom,load-addr", &load_addr);
	if (ret)
		return dev_err_probe(dev, ret, "no qcom,load-addr\n");
	core->load_addr = load_addr;

	ret = of_property_read_u32(dev->of_node, "qcom,max-frequency", &rate);
	if (ret)
		return dev_err_probe(dev, ret, "no qcom,max-frequency\n");
	core->core_rate = rate;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "vphys");
	if (!res)
		return -ENODEV;

	core->imem_size = resource_size(res);
	core->imem = devm_ioremap_resource(dev, res);
	if (IS_ERR(core->imem))
		return PTR_ERR(core->imem);

	ret = nss_clocks_get(core);
	if (ret)
		return ret;

	ret = devm_regulator_get_enable(dev, "npu");
	if (ret)
		return dev_err_probe(dev, ret, "npu supply\n");

	ret = nss_region_get(core);
	if (ret)
		return dev_err_probe(dev, ret, "no boot region\n");

	if (core->load_addr < core->region_base ||
	    core->load_addr >= core->region_base + core->region_size)
		return dev_err_probe(dev, -ERANGE,
				     "load address is outside the region\n");

	platform_set_drvdata(pdev, core);

	core->debugfs = debugfs_create_dir(dev_name(dev), NULL);
	debugfs_create_file("load", 0600, core->debugfs, core, &nss_load_fops);

	return 0;
}

static void nss_remove(struct platform_device *pdev)
{
	struct nss_core *core = platform_get_drvdata(pdev);

	debugfs_remove_recursive(core->debugfs);
}

static const struct of_device_id nss_of_match[] = {
	{ .compatible = "qcom,nss" },
	{ }
};
MODULE_DEVICE_TABLE(of, nss_of_match);

static struct platform_driver nss_driver = {
	.probe = nss_probe,
	.remove = nss_remove,
	.driver = {
		.name = "nss-wifi-drv",
		.of_match_table = nss_of_match,
	},
};
module_platform_driver(nss_driver);

MODULE_DESCRIPTION("NSS firmware host driver for the Wi-Fi data plane");
MODULE_LICENSE("GPL");
