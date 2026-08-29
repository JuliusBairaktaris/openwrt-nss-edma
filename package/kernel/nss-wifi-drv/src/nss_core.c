// SPDX-License-Identifier: GPL-2.0-only
/* Host driver for the NSS firmware behind the Wi-Fi data plane.
 *
 * Bringing a core up is: turn on its clocks and supply, clear the instruction
 * memory it starts executing out of, copy the firmware image into the
 * reserved region it runs from, satisfy the memory the image asks for, and
 * only then release it from reset. The order matters at one point in
 * particular - the image's memory requests carry the addresses the firmware
 * will use, so a core released before they are answered reads pointers that
 * were never written.
 *
 * None of it happens on probe. A running core takes over the switch CPU port,
 * so an image that started one while probing could not also bring up the host
 * ethernet stack; bring-up is asked for through debugfs, and a reboot returns
 * the board to the host data plane.
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
#include <linux/rtnetlink.h>
#include <linux/seq_file.h>
#include <linux/soc/qcom/qca_edma.h>

#include "nss_drv.h"

/* Core control registers, in the "nphys" range. */
#define NSS_CORE_RESET_CTRL		0x0004
#define NSS_CORE_BAR			0x0008
#define NSS_CORE_AMC			0x000c
#define NSS_CORE_BOOT_ADDR		0x0010
#define NSS_CORE_INT_STAT2_TYPE		0x0040
#define NSS_CORE_INT_STAT3_TYPE		0x0044
#define NSS_CORE_IFETCH_RANGE		0x0048

/* The core reaches its peripherals through a fixed aperture, and may fetch
 * instructions only from the window the image is linked into. Both are
 * properties of how the firmware was built.
 */
#define NSS_CORE_BAR_ADDR		0x3c000000
#define NSS_CORE_IFETCH_RANGE_ADDR	0xbf004001

/* Copy engine and core-to-core interrupts are level sensitive. */
#define NSS_CORE_INT_STAT2_LEVEL	0xffff
#define NSS_CORE_INT_STAT3_LEVEL	0xff

/* The two reset sets are released in order, and each core's bits sit one byte
 * apart in the register the cores share.
 */
#define NSS_MISC_RESET_FIRST		0x00000020
#define NSS_MISC_RESET_SECOND		0x00000017
#define NSS_MISC_RESET_SHIFT(id)	((id) << 3)

#define NSS_BOOT_TIMEOUT_MS		2000

/* Every clock the core needs, at the rate the firmware is built for. The
 * fabric clocks are shared between the two cores; a Wi-Fi data plane boots
 * only core 0, so they are taken from its node along with its own. The core
 * clock alone carries no rate here: the device tree states the frequencies
 * this SoC supports, and the core runs at the highest of them.
 */
/* The fabric clocks first, then the six that are the core's own. The split is
 * load-bearing rather than cosmetic: a core is given its own clocks back at
 * every start and loses them at every stop, while the fabric ones go on once
 * and stay on, because the switch sits behind them and a wired data plane
 * that has nothing to do with this driver goes down when they do.
 */
static const struct nss_clk_cfg nss_clk_tbl[] = {
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
	{ "nss-core-clk",			0 },
	{ "nss-ahb-clk",		200000000 },
	{ "nss-axi-clk",		461500000 },
	{ "nss-mpt-clk",		 25000000 },
	{ "nss-nc-axi-clk",		461500000 },
};

#define NSS_CLK_COUNT	ARRAY_SIZE(nss_clk_tbl)
#define NSS_CLK_FABRIC	13
#define NSS_CLK_OWN	(NSS_CLK_COUNT - NSS_CLK_FABRIC)

static int nss_clocks_get(struct nss_core *core)
{
	int i, ret;

	for (i = 0; i < NSS_CLK_COUNT; i++)
		core->clks[i].id = nss_clk_tbl[i].name;

	ret = devm_clk_bulk_get(core->dev, NSS_CLK_COUNT, core->clks);
	if (ret)
		return dev_err_probe(core->dev, ret, "clocks\n");

	for (i = 0; i < NSS_CLK_COUNT; i++) {
		unsigned long rate = nss_clk_tbl[i].rate ? : core->core_rate;

		ret = clk_set_rate(core->clks[i].clk, rate);
		if (ret)
			return dev_err_probe(core->dev, ret, "%s rate\n",
					     nss_clk_tbl[i].name);
	}

	return 0;
}

/* Turn the core's clocks on once and leave them on.
 *
 * Thirteen of the nineteen are fabric clocks the switch is behind, not the
 * core's own, so taking them away between one boot and the next disturbs a
 * data plane that has nothing to do with this driver. They go off when the
 * device does.
 */
static int nss_clocks_enable(struct nss_core *core)
{
	int ret;

	if (!core->clocks_on) {
		ret = clk_bulk_prepare_enable(NSS_CLK_FABRIC, core->clks);
		if (ret)
			return dev_err_probe(core->dev, ret, "fabric clocks\n");

		core->clocks_on = true;
	}

	ret = clk_bulk_prepare_enable(NSS_CLK_OWN, &core->clks[NSS_CLK_FABRIC]);
	if (ret)
		return dev_err_probe(core->dev, ret, "core clocks\n");

	return 0;
}

static void nss_clocks_disable(struct nss_core *core)
{
	if (!core->clocks_on)
		return;

	clk_bulk_disable_unprepare(NSS_CLK_FABRIC, core->clks);
	core->clocks_on = false;
}

/* The firmware is linked to run from a fixed address, so it is copied to that
 * address rather than anywhere within the region. The region the device tree
 * reserved is what bounds the copy: an image larger than the space between
 * its load address and the end of the region would run into whatever follows.
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

/* A running core takes CPU-port delivery away from the host, and the EDMA
 * conduit is what takes it back. There is one on this SoC, and the switch is
 * behind it, so it is found rather than described.
 */
static struct net_device *nss_conduit_get(void)
{
	struct net_device *dev;

	rtnl_lock();
	for_each_netdev(&init_net, dev) {
		if (qca_edma_netdev_is_conduit(dev)) {
			dev_hold(dev);
			rtnl_unlock();
			return dev;
		}
	}
	rtnl_unlock();

	return NULL;
}

static void nss_core_release(struct nss_core *core)
{
	u32 val;

	val = readl(core->misc_reset);
	val &= ~(NSS_MISC_RESET_FIRST << NSS_MISC_RESET_SHIFT(core->id));
	writel(val, core->misc_reset);

	/* The core needs 10 to 20 cycles after its reset clamp is released
	 * before the second set may follow.
	 */
	usleep_range(10, 20);

	val &= ~(NSS_MISC_RESET_SECOND << NSS_MISC_RESET_SHIFT(core->id));
	writel(val, core->misc_reset);

	/* Hold the core while its address configuration is written, so it
	 * cannot fetch from a window it has not been given yet.
	 */
	writel(1, core->nphys + NSS_CORE_RESET_CTRL);

	writel(1, core->nphys + NSS_CORE_AMC);
	writel(NSS_CORE_BAR_ADDR, core->nphys + NSS_CORE_BAR);
	writel(core->load_addr, core->nphys + NSS_CORE_BOOT_ADDR);

	writel(NSS_CORE_INT_STAT2_LEVEL, core->nphys + NSS_CORE_INT_STAT2_TYPE);
	writel(NSS_CORE_INT_STAT3_LEVEL, core->nphys + NSS_CORE_INT_STAT3_TYPE);

	writel(NSS_CORE_IFETCH_RANGE_ADDR, core->nphys + NSS_CORE_IFETCH_RANGE);

	writel(0, core->nphys + NSS_CORE_RESET_CTRL);
}

/* Stop the core before anything it is using is taken away. A core left
 * executing keeps driving the switch CPU port and keeps fetching, so a later
 * probe would copy a new image over live firmware and trap it; taking its
 * clocks first would leave a gated register behind, which resets the SoC.
 * Reset is asserted in the reverse order it was released, and only then is
 * the memory it was reading released.
 */
/* Stop a core and put back everything it was using.
 *
 * Asked for by hand as well as at teardown, because a WLAN driver holding a
 * reference to this module means unloading it is no longer a way to stop the
 * firmware - and stopping the firmware is what returns the switch block to
 * the state a cold boot left it in. Idempotent: a core that is not loaded has
 * nothing to put back.
 */
static void nss_core_halt(struct nss_core *core)
{
	u32 val;

	if (!core->loaded)
		return;

	writel(1, core->nphys + NSS_CORE_RESET_CTRL);

	val = readl(core->misc_reset);
	val |= NSS_MISC_RESET_SECOND << NSS_MISC_RESET_SHIFT(core->id);
	writel(val, core->misc_reset);

	val |= NSS_MISC_RESET_FIRST << NSS_MISC_RESET_SHIFT(core->id);
	writel(val, core->misc_reset);

	core->running = false;

	nss_log_shadow_stop(core);
	nss_rings_stop(core);
	nss_iface_unbind(core);

	/* After the rings, because a receive poll still running would find the
	 * conduit gone underneath it. The core is in reset by now, so the
	 * firmware-owned rings may be written: this returns the whole block to
	 * its cold-boot state rather than only the queue table, and a later
	 * boot starts from the same place this one did.
	 */
	if (core->conduit) {
		qca_edma_fw_baseline_restore(core->conduit);
		dev_put(core->conduit);
		core->conduit = NULL;
	}

	nss_mem_free_all(core);
	clk_bulk_disable_unprepare(NSS_CLK_OWN, &core->clks[NSS_CLK_FABRIC]);

	core->loaded = false;
	core->wifili_probed = false;
	core->cpu_port_taken = false;
}

static int nss_core_boot(struct nss_core *core)
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

	ret = nss_meminfo_init(core);
	if (ret)
		return ret;

	ret = nss_rings_start(core);
	if (ret)
		goto free;

	/* Taken before the core is released rather than after: a core that is
	 * running with nowhere to hand the CPU port back to leaves the board
	 * with no wired path at all, and no way to ask for one.
	 */
	core->conduit = nss_conduit_get();
	if (!core->conduit) {
		ret = -ENODEV;
		dev_err(core->dev, "no EDMA conduit\n");
		goto stop;
	}

	nss_iface_bind(core);

	core->cpu_port_taken = true;
	core->loaded = true;
	nss_log_shadow_start(core);

	/* A boot that timed out and was then answered leaves the completion
	 * signalled, and the next boot would take that for its own answer.
	 */
	reinit_completion(&core->booted);
	nss_core_release(core);

	if (!wait_for_completion_timeout(&core->booted,
					 msecs_to_jiffies(NSS_BOOT_TIMEOUT_MS))) {
		dev_err(core->dev, "core %u did not answer in %u ms\n",
			core->id, NSS_BOOT_TIMEOUT_MS);
		nss_log_dump(core, "boot timeout");
		nss_core_halt(core);
		return -ETIMEDOUT;
	}

	return 0;

stop:
	nss_rings_stop(core);
free:
	nss_mem_free_all(core);
	clk_bulk_disable_unprepare(NSS_CLK_OWN, &core->clks[NSS_CLK_FABRIC]);

	return ret;
}

static int nss_boot_set(void *data, u64 val)
{
	struct nss_core *core = data;

	if (val)
		return nss_core_boot(core);

	guard(mutex)(&core->lock);
	nss_core_halt(core);

	return 0;
}

static int nss_boot_get(void *data, u64 *val)
{
	struct nss_core *core = data;

	guard(mutex)(&core->lock);
	*val = core->running;

	return 0;
}

DEFINE_DEBUGFS_ATTRIBUTE(nss_boot_fops, nss_boot_get, nss_boot_set, "%llu\n");

static int nss_log_show(struct seq_file *s, void *unused)
{
	struct nss_core *core = s->private;

	guard(mutex)(&core->lock);

	return nss_log_show_ring(core, s);
}
DEFINE_SHOW_ATTRIBUTE(nss_log);

static int nss_probe_show(struct seq_file *s, void *unused)
{
	struct nss_core *core = s->private;

	if (!core->running)
		return -ENODEV;

	return nss_msg_probe(core, s);
}
DEFINE_SHOW_ATTRIBUTE(nss_probe);

/* What the firmware has actually delivered, by the interface number it named
 * and by the kind of buffer it arrived in. Which interface numbers a running
 * firmware sends on is not something the host can look up, and the exception
 * path has to be written against the ones it really uses.
 */
static int nss_rx_show(struct seq_file *s, void *unused)
{
	struct nss_core *core = s->private;
	int i;

	for (i = 0; i < ARRAY_SIZE(core->rx_type); i++)
		if (core->rx_type[i])
			seq_printf(s, "type %d: %llu\n", i, core->rx_type[i]);

	for (i = 0; i < NSS_INTERFACE_MAX; i++)
		if (core->rx_iface[i])
			seq_printf(s, "interface %d: %llu\n", i,
				   core->rx_iface[i]);

	seq_printf(s, "notify: %llu link-desc seen: %llu returned: %llu\n",
		   core->notify, core->link_desc_seen,
		   core->link_desc_returned);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(nss_rx);

static int nss_wifili_show(struct seq_file *s, void *unused)
{
	return nss_wifili_start(s);
}
DEFINE_SHOW_ATTRIBUTE(nss_wifili);

/* Leave the switch CPU port with the firmware instead of taking it back.
 *
 * The two data planes share the block and which of them owns the port is a
 * runtime property, so it is one the host can set. Written before a core
 * boots, this leaves wired traffic arriving on the firmware's data queues,
 * which is the only way to exercise the exception path before there is a
 * radio to exception from.
 */
static int nss_fwport_set(void *data, u64 val)
{
	struct nss_core *core = data;

	guard(mutex)(&core->lock);
	core->cpu_port_to_fw = val;

	return 0;
}

static int nss_fwport_get(void *data, u64 *val)
{
	struct nss_core *core = data;

	guard(mutex)(&core->lock);
	*val = core->cpu_port_to_fw;

	return 0;
}

DEFINE_DEBUGFS_ATTRIBUTE(nss_fwport_fops, nss_fwport_get, nss_fwport_set,
			 "%llu\n");

/* The region the core boots from is described once, on the node the two cores
 * share, and each core's load address points into it.
 */
static int nss_region_get(struct nss_core *core)
{
	struct device_node *cmn __free(device_node) =
		of_find_compatible_node(NULL, NULL, "qcom,nss-common");
	struct device_node *mem;
	struct reserved_mem *rmem;
	struct resource res;
	int ret;

	if (!cmn)
		return -ENODEV;

	ret = of_address_to_resource(cmn, 0, &res);
	if (ret)
		return ret;

	/* The reset register sits inside the range the clock controller
	 * claims, so it is mapped without being requested; asking for it
	 * would be refused.
	 */
	core->misc_reset = devm_ioremap(core->dev, res.start,
					resource_size(&res));
	if (!core->misc_reset)
		return -ENOMEM;

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

	core = devm_kzalloc(dev, struct_size(core, clks, NSS_CLK_COUNT),
			    GFP_KERNEL);
	if (!core)
		return -ENOMEM;

	core->dev = dev;
	INIT_LIST_HEAD(&core->allocs);
	init_completion(&core->booted);

	ret = devm_mutex_init(dev, &core->lock);
	if (ret)
		return ret;

	/* The firmware addresses everything it is given with 32 bits, so
	 * every block handed to it has to come from below 4 GB.
	 */
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return dev_err_probe(dev, ret, "no 32-bit DMA\n");

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

	core->nphys = devm_platform_ioremap_resource_byname(pdev, "nphys");
	if (IS_ERR(core->nphys))
		return PTR_ERR(core->nphys);

	/* Shared with the other core and with the interrupt controller, so it
	 * is mapped rather than claimed.
	 */
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "qgic-phys");
	if (!res)
		return -ENODEV;

	core->qgic = devm_ioremap(dev, res->start, resource_size(res));
	if (!core->qgic)
		return -ENOMEM;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "vphys");
	if (!res)
		return -ENODEV;

	core->imem_base = res->start;
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

	ret = nss_log_shadow_init(core);
	if (ret)
		return ret;

	ret = nss_msg_init(core);
	if (ret)
		return ret;

	ret = nss_region_get(core);
	if (ret)
		return dev_err_probe(dev, ret, "no boot region\n");

	if (core->load_addr < core->region_base ||
	    core->load_addr >= core->region_base + core->region_size)
		return dev_err_probe(dev, -ERANGE,
				     "load address is outside the region\n");

	platform_set_drvdata(pdev, core);
	nss_wifili_bind(core);

	core->debugfs = debugfs_create_dir(dev_name(dev), NULL);
	debugfs_create_file("boot", 0600, core->debugfs, core, &nss_boot_fops);
	debugfs_create_file("log", 0400, core->debugfs, core, &nss_log_fops);
	debugfs_create_file("msg_probe", 0400, core->debugfs, core,
			    &nss_probe_fops);
	debugfs_create_file("rx", 0400, core->debugfs, core, &nss_rx_fops);
	debugfs_create_file("wifili_start", 0400, core->debugfs, core,
			    &nss_wifili_fops);
	debugfs_create_file("cpu_port_to_fw", 0600, core->debugfs, core,
			    &nss_fwport_fops);

	return 0;
}

static void nss_remove(struct platform_device *pdev)
{
	struct nss_core *core = platform_get_drvdata(pdev);

	nss_wifili_bind(NULL);
	debugfs_remove_recursive(core->debugfs);
	nss_core_halt(core);
	nss_clocks_disable(core);
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
