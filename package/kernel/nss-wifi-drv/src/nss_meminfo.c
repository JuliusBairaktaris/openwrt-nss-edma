// SPDX-License-Identifier: GPL-2.0-only
/* Satisfying the firmware's memory requests.
 *
 * The image carries a table of the memory it wants, because its heap and
 * table sizes change from one firmware line to the next and the host has no
 * way to know them. The host walks that table, allocates each block and
 * writes the address back into the request. There is no negotiation and no
 * error channel: the firmware reads the address it is given and uses it, so a
 * block left unsatisfied is a coprocessor writing into whatever happens to be
 * at that address.
 *
 * Two of the blocks are also the host's own: the page describing the message
 * rings, whose contents the host fills in, and the log ring, which is how the
 * firmware is read after it fails.
 */

#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/slab.h>

#include "nss_drv.h"

/* The reserve area at the head of the image points at the request table. */
struct nss_meminfo_reserve {
	u32 magic;
	u32 map_addr;
};

static void *nss_mem_alloc(struct nss_core *core, size_t size, u32 align,
			   u32 *addr_out)
{
	struct nss_alloc *a;
	dma_addr_t dma;
	void *cpu;

	a = devm_kzalloc(core->dev, sizeof(*a), GFP_KERNEL);
	if (!a)
		return NULL;

	/* Coherent memory rather than streaming plus hand-written cache
	 * maintenance. The firmware cannot tell the difference, and the
	 * alternative is a maintenance call at every ring index update that
	 * has to be right in both directions every time.
	 */
	cpu = dma_alloc_coherent(core->dev, ALIGN(size, align), &dma,
				 GFP_KERNEL);
	if (!cpu)
		return NULL;

	a->cpu = cpu;
	a->dma = dma;
	a->size = ALIGN(size, align);
	list_add_tail(&a->node, &core->allocs);

	*addr_out = (u32)dma;

	return cpu;
}

void nss_mem_free_all(struct nss_core *core)
{
	struct nss_alloc *a, *tmp;

	list_for_each_entry_safe(a, tmp, &core->allocs, node) {
		dma_free_coherent(core->dev, a->size, a->cpu, a->dma);
		list_del(&a->node);
	}
}

/* IMEM is handed out by bumping a pointer through the window the core boots
 * with. It is on-chip memory reached through a device mapping, so it is
 * cleared with the accessor for that and never handed back as an ordinary
 * pointer.
 */
static int nss_imem_alloc(struct nss_core *core, size_t size, u32 align,
			  u32 *addr_out)
{
	u32 off = ALIGN(core->imem_used, align);

	if (off + size > core->imem_size)
		return -ENOMEM;

	memset_io(core->imem + off, 0, size);

	core->imem_used = off + size;
	*addr_out = core->imem_base + off;

	return 0;
}

/* The request record, by offset, because it is read and written through an
 * uncached mapping of the region the image was copied into rather than as a
 * structure the compiler may reorder accesses to.
 */
#define REQ_MAGIC	0
#define REQ_NAME	2
#define REQ_MEMTYPE	50
#define REQ_ALIGNMENT	56
#define REQ_SIZE	60
#define REQ_ADDR	64
#define REQ_STRIDE	68

static int nss_meminfo_satisfy(struct nss_core *core, void __iomem *r)
{
	char name[NSS_MEMINFO_BLOCK_NAME_LEN];
	u32 size = readl(r + REQ_SIZE);
	u16 memtype = readw(r + REQ_MEMTYPE);
	u32 align = readl(r + REQ_ALIGNMENT);
	void *cpu = NULL;
	bool host_reads;
	u32 addr;
	int ret;

	memcpy_fromio(name, r + REQ_NAME, sizeof(name));
	name[sizeof(name) - 1] = '\0';

	if (!align)
		align = SMP_CACHE_BYTES;

	/* Two of the blocks are read and written by the host as ordinary
	 * structures, so they have to come from ordinary memory whatever the
	 * image asked for. Where a block lives is the host's choice; the
	 * firmware only reads the address it is handed.
	 */
	host_reads = !strcmp(name, "nss_if_mem_map_inst") ||
		     !strcmp(name, "debug_boot_log_desc");

	if (host_reads && memtype == NSS_MEMINFO_MEMTYPE_IMEM) {
		dev_info(core->dev, "%s moved to sdram\n", name);
		memtype = NSS_MEMINFO_MEMTYPE_SDRAM;
	}

	switch (memtype) {
	case NSS_MEMINFO_MEMTYPE_IMEM:
		ret = nss_imem_alloc(core, size, align, &addr);
		if (ret) {
			dev_err(core->dev, "meminfo %s: %u bytes of imem\n",
				name, size);
			return ret;
		}
		writel(addr, r + REQ_ADDR);
		return 0;
	case NSS_MEMINFO_MEMTYPE_SDRAM:
		cpu = nss_mem_alloc(core, size, align, &addr);
		break;
	case NSS_MEMINFO_MEMTYPE_INFO:
		/* Not an allocation: the firmware is asking a question and
		 * reads the answer back out of the same field.
		 */
		if (!strcmp(name, "heap_ddr_size")) {
			writel(core->region_size, r + REQ_ADDR);
			return 0;
		}
		dev_warn(core->dev, "unanswered meminfo query %s\n", name);
		return 0;
	default:
		dev_err(core->dev, "meminfo %s wants memtype %u\n", name,
			memtype);
		return -EINVAL;
	}

	if (!cpu) {
		dev_err(core->dev, "meminfo %s: %u bytes unavailable\n",
			name, size);
		return -ENOMEM;
	}

	memset(cpu, 0, size);
	writel(addr, r + REQ_ADDR);

	/* The two blocks the host reads or writes itself have to be kept, not
	 * just allocated: everything else the firmware owns outright.
	 */
	if (!strcmp(name, "nss_if_mem_map_inst"))
		core->if_map = cpu;
	else if (!strcmp(name, "debug_boot_log_desc")) {
		core->log = cpu;
		/* How many entries the ring can hold follows from the block
		 * the firmware asked for, so the count it later reports has
		 * something the host measured to be checked against.
		 */
		core->log_entries = (size - sizeof(struct nss_log_descriptor)) /
				    sizeof(struct nss_log_entry);
	}

	dev_dbg(core->dev, "meminfo %s: %u bytes at %#x\n", name, size, addr);

	return 0;
}

/* Place the message rings. These are not firmware requests - the host decides
 * how many rings there are and how large, and publishes that in the index
 * page.
 */
static int nss_meminfo_rings(struct nss_core *core)
{
	size_t h2n_sz = sizeof(struct h2n_descriptor) * NSS_H2N_RINGS *
			NSS_RING_STRIDE_ENTRIES;
	size_t n2h_sz = sizeof(struct n2h_descriptor) * NSS_N2H_RINGS *
			NSS_RING_STRIDE_ENTRIES;
	u32 h2n_addr, n2h_addr;
	void *h2n, *n2h;
	int i;

	h2n = nss_mem_alloc(core, h2n_sz, SMP_CACHE_BYTES, &h2n_addr);
	n2h = nss_mem_alloc(core, n2h_sz, SMP_CACHE_BYTES, &n2h_addr);
	if (!h2n || !n2h)
		return -ENOMEM;

	memset(h2n, 0, h2n_sz);
	memset(n2h, 0, n2h_sz);

	for (i = 0; i < NSS_H2N_RINGS; i++) {
		u32 off = i * sizeof(struct h2n_descriptor) *
			  NSS_RING_STRIDE_ENTRIES;

		core->h2n[i].desc = h2n + off;
		spin_lock_init(&core->h2n[i].lock);
		core->if_map->h2n_desc_if[i].desc_addr = h2n_addr + off;
		core->if_map->h2n_desc_if[i].size = NSS_RING_ENTRIES;
	}

	for (i = 0; i < NSS_N2H_RINGS; i++) {
		u32 off = i * sizeof(struct n2h_descriptor) *
			  NSS_RING_STRIDE_ENTRIES;

		core->n2h[i].desc = n2h + off;
		core->if_map->n2h_desc_if[i].desc_addr = n2h_addr + off;
		core->if_map->n2h_desc_if[i].size = NSS_RING_ENTRIES;
	}

	core->if_map->h2n_rings = NSS_H2N_RINGS;
	core->if_map->n2h_rings = NSS_N2H_RINGS;

	return 0;
}

int nss_meminfo_init(struct nss_core *core)
{
	struct nss_meminfo_reserve __iomem *reserve;
	u32 map_addr, magic;
	void __iomem *map;
	size_t off;
	int ret = 0;

	reserve = ioremap(core->load_addr + NSS_MEMINFO_OFFSET,
			  sizeof(*reserve));
	if (!reserve)
		return -ENOMEM;

	magic = readl(&reserve->magic);
	map_addr = readl(&reserve->map_addr);
	iounmap(reserve);

	if (magic != NSS_MEMINFO_MAGIC_RESERVE) {
		dev_err(core->dev, "image has no meminfo table (%#x)\n", magic);
		return -EINVAL;
	}

	/* The table lives inside the region the image was copied into, so it
	 * is bounded by that region rather than by a length of its own.
	 */
	if (map_addr < core->region_base ||
	    map_addr >= core->region_base + core->region_size)
		return -ERANGE;

	map = ioremap(map_addr, PAGE_SIZE);
	if (!map)
		return -ENOMEM;

	if (readl(map) != NSS_MEMINFO_MAGIC_MAP) {
		dev_err(core->dev, "meminfo table is not one (%#x)\n",
			readl(map));
		ret = -EINVAL;
		goto out;
	}

	/* Requests follow the map magic and run until the end marker. The
	 * walk is bounded by the page that was mapped, so a table whose end
	 * marker the firmware never wrote cannot run past it.
	 */
	off = sizeof(u32);

	while (off + REQ_STRIDE <= PAGE_SIZE &&
	       readw(map + off + REQ_MAGIC) == NSS_MEMINFO_MAGIC_REQUEST) {
		ret = nss_meminfo_satisfy(core, map + off);
		if (ret)
			goto out;

		off += REQ_STRIDE;
	}

	if (off + sizeof(u16) > PAGE_SIZE ||
	    readw(map + off) != NSS_MEMINFO_MAGIC_END) {
		dev_err(core->dev, "meminfo table has no end\n");
		ret = -EINVAL;
		goto out;
	}

	if (!core->if_map) {
		dev_err(core->dev, "firmware asked for no interface map\n");
		ret = -EINVAL;
		goto out;
	}

	ret = nss_meminfo_rings(core);
	if (ret)
		goto out;

	core->if_map->magic = NSS_IF_MEM_MAP_MAGIC;
	core->if_map->if_version = NSS_IF_MEM_MAP_VERSION;

out:
	iounmap(map);

	return ret;
}
