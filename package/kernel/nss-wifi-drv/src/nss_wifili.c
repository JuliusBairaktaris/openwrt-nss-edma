// SPDX-License-Identifier: GPL-2.0-only
/* Bringing wifili up against rings the host owns.
 *
 * The initialisation message is the largest structure in this ABI and the one
 * with the most room to be silently misread: a field whose offset moved
 * between firmware lines is accepted, stored, and produces a data plane that
 * does not work for reasons nothing reports. It can be checked without a
 * radio, and it is checked here before there is one.
 *
 * The method is refusal. The firmware bounds-checks several fields before it
 * touches anything, so a message deliberately invalid in exactly one of them
 * is rejected without allocating - and it can only be rejected if the
 * firmware read that value where the host put it. Each arm below therefore
 * confirms one offset, and the valid arm that follows them confirms that the
 * whole structure is acceptable.
 *
 * The rings are the host's own coherent memory and nothing consumes them.
 * That is enough for every check here, because what is under test is the
 * message rather than the data plane.
 */

#include <linux/container_of.h>
#include <linux/dma-mapping.h>
#include <linux/etherdevice.h>
#include <linux/if_ether.h>
#include <linux/seq_file.h>
#include <linux/slab.h>

#include "nss_drv.h"
#include "nss_cmn.h"
#include "nss_dynamic_interface.h"
#include "nss_shaper.h"
#include "nss_if.h"
#include "nss_wifi_vdev.h"
#include "nss_wifili_if.h"
#include <linux/soc/qcom/nss_wifi.h>

/* Small on purpose. Nothing drains these, and the receive ring the firmware
 * fills from its own pool is the one that would cost something if it were not.
 */
#define NSS_WIFILI_RING_ENTRIES		32
#define NSS_WIFILI_ENTRY_WORDS		8
/* Transmit descriptors per pool. The firmware holds one for every frame in
 * flight and refuses the frame when it cannot get one, silently as far as the
 * host is concerned - only the firmware's own desc_alloc_fail counter shows
 * it, and at 64 it read three times the number of frames that got through.
 * A descriptor is under 128 bytes, so this many still fit the page below.
 */
#define NSS_WIFILI_TX_DESC		512

/* One page for the descriptors and one for the extension descriptors: the two
 * kinds are carved at different strides out of whichever page each starts on,
 * so a shared page leaves one of the pools without one.
 */
#define NSS_WIFILI_TX_DESC_MEM		SZ_64K

/* The receive buffer the hardware would be programmed with, and the tag block
 * in front of it. Both are bounds-checked by the firmware, which is why they
 * are here rather than left zero.
 */
#define NSS_WIFILI_TLV_SIZE		256
#define NSS_WIFILI_RX_BUF_LEN		2048

/* This SoC, as the firmware enumerates targets. */
#define NSS_WIFILI_TARGET_QCA8074V2	24

/* Where the per-radio rings begin, which is what tells the firmware to read
 * their write pointer out of the second shadow array rather than a register.
 */
#define NSS_WIFILI_LMAC_RING_START	128

enum nss_wifili_ring_id {
	NSS_WIFILI_RING_TCL,
	NSS_WIFILI_RING_TX_COMP,
	NSS_WIFILI_RING_REO_DEST,
	NSS_WIFILI_RING_REO_REINJECT,
	NSS_WIFILI_RING_RX_REL,
	NSS_WIFILI_RING_REO_EXCEPTION,
	NSS_WIFILI_RING_MAX,
};

/* Which of the deliberate faults to introduce, or none. */
enum nss_wifili_fault {
	NSS_WIFILI_FAULT_NONE,
	NSS_WIFILI_FAULT_RING_ALIGN,
	NSS_WIFILI_FAULT_RING_DIR,
	NSS_WIFILI_FAULT_RING_ENTRIES,
	NSS_WIFILI_FAULT_TARGET,
	NSS_WIFILI_FAULT_TLV_SIZE,
	NSS_WIFILI_FAULT_RX_BUF_LEN,
};

struct nss_wifili_mem {
	void *cpu;
	dma_addr_t dma;
	size_t size;
};

struct nss_wifili_ctx {
	struct nss_wifili_mem ring[NSS_WIFILI_RING_MAX];
	struct nss_wifili_mem shadow;
	struct nss_wifili_mem doorbell;
	struct nss_wifili_mem txdesc;
	struct nss_wifili_mem txdescext;
};

/* The one core that runs wifili, and what the WLAN driver said about its
 * rings. The WLAN driver is loaded and unloaded independently of this one and
 * knows nothing about which core is which, so the binding lives here.
 */
static struct nss_core *nss_wifili_core;
static struct nss_wifi_soc nss_wifili_soc;
static struct nss_wifi_pdev nss_wifili_pdev[NSS_WIFILI_MAX_PDEV_NUM_MSG];
static bool nss_wifili_have_soc;
static u8 nss_wifili_pdevs;
static int nss_wifili_radio_if[NSS_WIFILI_MAX_PDEV_NUM_MSG];
static DEFINE_MUTEX(nss_wifili_lock);

void nss_wifili_bind(struct nss_core *core)
{
	guard(mutex)(&nss_wifili_lock);
	nss_wifili_core = core;
}

/* Keep what the WLAN driver says; starting the firmware is a separate act,
 * because a running core takes the hardware with it.
 */
int nss_wifi_soc_register(const struct nss_wifi_soc *soc)
{
	guard(mutex)(&nss_wifili_lock);

	if (soc->num_tcl > NSS_WIFI_MAX_TCL_RINGS ||
	    soc->num_reo > NSS_WIFI_MAX_REO_RINGS)
		return -EINVAL;

	nss_wifili_soc = *soc;
	nss_wifili_pdevs = 0;
	nss_wifili_have_soc = true;

	return 0;
}
EXPORT_SYMBOL_GPL(nss_wifi_soc_register);

void nss_wifi_soc_unregister(void)
{
	guard(mutex)(&nss_wifili_lock);
	nss_wifili_have_soc = false;
	nss_wifili_pdevs = 0;

	/* The WLAN driver is about to free the rings this reaches into, and a
	 * receive poll that read the callback a moment ago is still holding
	 * it, so the withdrawal is not complete until that poll has ended.
	 */
	WRITE_ONCE(nss_wifili_soc.link_desc_return, NULL);
	if (nss_wifili_core)
		nss_rings_quiesce(nss_wifili_core);
}
EXPORT_SYMBOL_GPL(nss_wifi_soc_unregister);

int nss_wifi_pdev_register(const struct nss_wifi_pdev *pdev)
{
	guard(mutex)(&nss_wifili_lock);

	if (!nss_wifili_have_soc || nss_wifili_pdevs >= ARRAY_SIZE(nss_wifili_pdev))
		return -EINVAL;

	nss_wifili_pdev[nss_wifili_pdevs++] = *pdev;

	return 0;
}
EXPORT_SYMBOL_GPL(nss_wifi_pdev_register);

/* The one message the firmware sends unasked that must be acted on.
 *
 * It reaps rings holding MSDU-link descriptors it cannot put back, because
 * the ring that returns one to the idle list stays with the WLAN driver, and
 * hands each one back here instead. An unreturned descriptor is gone from the
 * idle list for good, and an empty idle list stops receive on every radio
 * rather than only on the path that emptied it - so late is the same as never
 * and this may not be deferred.
 */
void nss_wifili_notify(struct nss_core *core, const struct nss_cmn_msg *ncm,
		       u32 len)
{
	void (*home)(void *priv, const u32 *buf_addr_info);
	const struct nss_wifili_msg *m;

	if (ncm->type != NSS_WIFILI_LINK_DESC_INFO_MSG ||
	    len < offsetof(struct nss_wifili_msg, msg) +
		  sizeof(struct nss_wifili_soc_linkdesc_buf_info_msg))
		return;

	/* Counted before the way home is looked up, because whether the
	 * firmware sends these at all, and how often, is what this rung is
	 * measuring: there is no receive to lose yet, and no route back until
	 * the shape of what arrives has been read off the device.
	 */
	core->link_desc_seen++;

	home = READ_ONCE(nss_wifili_soc.link_desc_return);
	if (!home)
		return;

	m = container_of_const(ncm, struct nss_wifili_msg, cm);
	home(nss_wifili_soc.priv, (const u32 *)&m->msg.linkdescinfomsg);
	core->link_desc_returned++;
}

static void nss_wifili_ring_fill(struct nss_wifili_hal_srng_info *r,
				 const struct nss_wifi_ring *src)
{
	r->ring_id = src->id;
	r->ring_dir = src->dir;
	r->num_entries = src->num_entries;
	r->entry_size = src->entry_size;
	r->flags = src->flags;
	r->ring_base_paddr = src->base;
	r->hwreg_base[0] = src->hwreg[0];
	r->hwreg_base[1] = src->hwreg[1];
}

static int nss_wifili_mem_get(struct nss_core *core, struct nss_wifili_mem *m,
			      size_t size)
{
	m->size = size;
	m->cpu = dmam_alloc_coherent(core->dev, size, &m->dma, GFP_KERNEL);

	return m->cpu ? 0 : -ENOMEM;
}

static void nss_wifili_srng(struct nss_wifili_hal_srng_info *r,
			    const struct nss_wifili_ctx *w,
			    enum nss_wifili_ring_id id, u32 dir)
{
	r->ring_id = id;
	r->ring_base_paddr = w->ring[id].dma;
	r->num_entries = NSS_WIFILI_RING_ENTRIES;
	r->entry_size = NSS_WIFILI_ENTRY_WORDS;
	r->ring_dir = dir;
	r->flags = 0;

	/* The firmware writes the ring's head or tail through this address as
	 * if it were a register. Nothing here has a register to give it, so it
	 * gets a page of the host's own that no one reads.
	 */
	r->hwreg_base[0] = w->doorbell.dma;
	r->hwreg_base[1] = w->doorbell.dma + id * 16;
}

static void nss_wifili_init_fill(struct nss_wifili_init_msg *init,
				 const struct nss_wifili_ctx *w,
				 enum nss_wifili_fault fault)
{
	init->hssm.shadow_rdptr_mem_addr = w->shadow.dma;
	init->hssm.shadow_wrptr_mem_addr = w->shadow.dma + SZ_1K;
	init->hssm.lmac_rings_start_id = NSS_WIFILI_LMAC_RING_START;

	init->num_tcl_data_rings = 1;
	init->num_reo_dest_rings = 1;

	nss_wifili_srng(&init->tcl_ring_info[0], w, NSS_WIFILI_RING_TCL, 0);
	nss_wifili_srng(&init->tx_comp_ring[0], w, NSS_WIFILI_RING_TX_COMP, 1);
	nss_wifili_srng(&init->reo_dest_ring[0], w, NSS_WIFILI_RING_REO_DEST, 1);
	nss_wifili_srng(&init->reo_reinject_ring, w,
			NSS_WIFILI_RING_REO_REINJECT, 0);
	nss_wifili_srng(&init->rx_rel_ring, w, NSS_WIFILI_RING_RX_REL, 1);
	nss_wifili_srng(&init->reo_exception_ring, w,
			NSS_WIFILI_RING_REO_EXCEPTION, 1);

	init->wtdim.num_tx_desc = NSS_WIFILI_TX_DESC;
	init->wtdim.num_tx_desc_ext = NSS_WIFILI_TX_DESC;
	init->wtdim.num_pool = 1;
	init->wtdim.num_memaddr = 1;
	init->wtdim.memory_addr[0] = w->txdesc.dma;
	init->wtdim.memory_size[0] = w->txdesc.size;
	init->wtdim.num_tx_device_limit = NSS_WIFILI_TX_DESC;

	init->target_type = NSS_WIFILI_TARGET_QCA8074V2;
	init->wrip.tlv_size = NSS_WIFILI_TLV_SIZE;
	init->wrip.rx_buf_len = NSS_WIFILI_RX_BUF_LEN;

	switch (fault) {
	case NSS_WIFILI_FAULT_RING_ALIGN:
		init->tcl_ring_info[0].ring_base_paddr |= 1;
		break;
	case NSS_WIFILI_FAULT_RING_DIR:
		init->tcl_ring_info[0].ring_dir = 2;
		break;
	case NSS_WIFILI_FAULT_RING_ENTRIES:
		init->tcl_ring_info[0].num_entries = 64 * 1024;
		break;
	case NSS_WIFILI_FAULT_TARGET:
		init->target_type = 0;
		break;
	case NSS_WIFILI_FAULT_TLV_SIZE:
		init->wrip.tlv_size = 8;
		break;
	case NSS_WIFILI_FAULT_RX_BUF_LEN:
		init->wrip.rx_buf_len = 4096;
		break;
	case NSS_WIFILI_FAULT_NONE:
		break;
	}
}

static int nss_wifili_alloc(struct nss_core *core, struct nss_wifili_ctx *w)
{
	size_t ring = NSS_WIFILI_RING_ENTRIES * NSS_WIFILI_ENTRY_WORDS * 4;
	int i, ret;

	for (i = 0; i < NSS_WIFILI_RING_MAX; i++) {
		ret = nss_wifili_mem_get(core, &w->ring[i], ring);
		if (ret)
			return ret;
	}

	ret = nss_wifili_mem_get(core, &w->shadow, SZ_4K);
	if (ret)
		return ret;

	ret = nss_wifili_mem_get(core, &w->doorbell, SZ_4K);
	if (ret)
		return ret;

	return nss_wifili_mem_get(core, &w->txdesc, NSS_WIFILI_TX_DESC_MEM);
}

static void nss_wifili_arm(struct nss_core *core, struct seq_file *s,
			   struct nss_wifili_msg *m,
			   const struct nss_wifili_ctx *w,
			   enum nss_wifili_fault fault, const char *what)
{
	int ret;

	memset(m, 0, sizeof(*m));
	m->cm.interface = NSS_INTERFACE_WIFILI;
	m->cm.type = NSS_WIFILI_INIT_MSG;
	nss_wifili_init_fill(&m->msg.init, w, fault);
	ret = nss_msg_send(core, m, offsetof(struct nss_wifili_msg, msg) +
				    sizeof(m->msg.init));

	seq_printf(s, "%-16s rc %d response %u error %u\n", what, ret,
		   m->cm.response, m->cm.error);
}

/* Each refusal confirms the offset of the one field it corrupted; the
 * acceptance that follows confirms the rest of a structure with a hundred and
 * fifty fields in it.
 */
int nss_wifili_probe(struct nss_core *core, struct seq_file *s)
{
	static const struct {
		enum nss_wifili_fault fault;
		const char *what;
	} arms[] = {
		{ NSS_WIFILI_FAULT_RING_ALIGN,	"ring alignment:" },
		{ NSS_WIFILI_FAULT_RING_DIR,	"ring direction:" },
		{ NSS_WIFILI_FAULT_RING_ENTRIES, "ring entries:" },
		/* Not refused on this firmware, which is a finding rather than
		 * a check: either the target is not validated here or the
		 * field is not where this header puts it.
		 */
		{ NSS_WIFILI_FAULT_TARGET,	"target type:" },
		{ NSS_WIFILI_FAULT_TLV_SIZE,	"tlv size:" },
		{ NSS_WIFILI_FAULT_RX_BUF_LEN,	"rx buffer len:" },
	};
	struct nss_wifili_ctx *w;
	struct nss_wifili_msg *m;
	int i, ret;

	/* Only the first run says anything. An initialisation the firmware
	 * accepted leaves the radio in a state the next message is judged
	 * against, so a second pass answers about the state machine rather
	 * than about the field it corrupted.
	 */
	if (core->wifili_probed) {
		seq_puts(s, "wifili:          already initialised, reload for a clean core\n");
		return 0;
	}

	/* Two kilobytes of union, which is more than a kernel stack frame is
	 * allowed to carry.
	 */
	m = kzalloc(sizeof(*m), GFP_KERNEL);
	w = kzalloc(sizeof(*w), GFP_KERNEL);
	if (!m || !w) {
		ret = -ENOMEM;
		goto out;
	}

	ret = nss_wifili_alloc(core, w);
	if (ret)
		goto out;

	for (i = 0; i < ARRAY_SIZE(arms); i++)
		nss_wifili_arm(core, s, m, w, arms[i].fault, arms[i].what);

	/* Last, because it is a one-way door: an initialisation the firmware
	 * accepts leaves the radio in a state only a full teardown returns
	 * from, and the teardown needs a radio that a host-invented ring set
	 * cannot bring up - the physical-device initialisation that follows
	 * this one programs hardware, and with rings the host made up it
	 * traps every thread. So the probe stops here, and a second run wants
	 * a fresh core.
	 */
	nss_wifili_arm(core, s, m, w, NSS_WIFILI_FAULT_NONE, "init:");
	core->wifili_probed = true;

	ret = 0;
out:
	kfree(w);
	kfree(m);

	return ret;
}

/* Give the firmware a node for one radio and return the number it chose. */
static int nss_wifili_radio_alloc(struct nss_core *core,
				  struct nss_wifili_msg *scratch)
{
	struct nss_dynamic_interface_msg d;
	int ret;

	memset(&d, 0, sizeof(d));
	d.cm.interface = NSS_INTERFACE_DYNAMIC;
	d.cm.type = NSS_DYNAMIC_INTERFACE_ALLOC_NODE;
	d.msg.alloc_node.type = NSS_DYNAMIC_INTERFACE_TYPE_WIFILI_INTERNAL;

	ret = nss_msg_send(core, &d, sizeof(d));
	if (ret)
		return ret;

	return d.msg.alloc_node.if_num;
}

/* Put one virtual device on a radio, and take it up.
 *
 * A radio carries no traffic on its own: a frame belongs to a virtual device,
 * and it is the vdev's interface number a received frame arrives under and
 * that a transmitted one is addressed to. This is the smallest thing that
 * shows the firmware will accept one. The numbers a real interface would
 * supply are left at zero, so whatever the firmware insists on shows up as a
 * refusal rather than as a silence.
 */
static void nss_wifili_vdev_probe(struct nss_core *core, struct seq_file *s,
				  int radio_if)
{
	struct nss_dynamic_interface_msg d;
	struct nss_wifi_vdev_msg *v;
	int if_num, ret;

	memset(&d, 0, sizeof(d));
	d.cm.interface = NSS_INTERFACE_DYNAMIC;
	d.cm.type = NSS_DYNAMIC_INTERFACE_ALLOC_NODE;
	d.msg.alloc_node.type = NSS_DYNAMIC_INTERFACE_TYPE_VAP;
	ret = nss_msg_send(core, &d, sizeof(d));
	if_num = d.msg.alloc_node.if_num;
	seq_printf(s, "vdev node:     rc %d response %u if_num %d\n", ret,
		   d.cm.response, if_num);
	if (ret)
		return;

	v = kzalloc(sizeof(*v), GFP_KERNEL);
	if (!v)
		return;

	v->cm.interface = if_num;
	v->cm.type = NSS_WIFI_VDEV_INTERFACE_CONFIGURE_MSG;
	v->msg.vdev_config.radio_ifnum = radio_if;
	eth_random_addr(v->msg.vdev_config.mac_addr);
	ret = nss_msg_send(core, v, offsetof(struct nss_wifi_vdev_msg, msg) +
				    sizeof(v->msg.vdev_config));
	seq_printf(s, "vdev config:   rc %d response %u error %u\n", ret,
		   v->cm.response, v->cm.error);

	if (!ret) {
		memset(v, 0, sizeof(*v));
		v->cm.interface = if_num;
		v->cm.type = NSS_WIFI_VDEV_INTERFACE_UP_MSG;
		ret = nss_msg_send(core, v,
				   offsetof(struct nss_wifi_vdev_msg, msg) +
				   sizeof(v->msg.vdev_config));
		seq_printf(s, "vdev up:       rc %d response %u error %u\n",
			   ret, v->cm.response, v->cm.error);

		/* Down before the node goes: a virtual device released while
		 * it is still up faults the firmware.
		 */
		memset(v, 0, sizeof(*v));
		v->cm.interface = if_num;
		v->cm.type = NSS_WIFI_VDEV_INTERFACE_DOWN_MSG;
		ret = nss_msg_send(core, v,
				   offsetof(struct nss_wifi_vdev_msg, msg) +
				   sizeof(v->msg.vdev_config));
		seq_printf(s, "vdev down:     rc %d response %u error %u\n",
			   ret, v->cm.response, v->cm.error);
	}

	/* The node is deliberately left allocated. Releasing it is what the
	 * next arm has to answer for; this one is about whether a virtual
	 * device can be made at all, and the probe runs once per core.
	 */
	seq_puts(s, "vdev node left allocated\n");

	kfree(v);
}

/* Start the firmware's Wi-Fi data plane on the rings the WLAN driver owns.
 *
 * This is where the ring set stops being invented. Everything the probe above
 * could not reach - the physical-device initialisation, which programs
 * hardware, and the start that puts the firmware's workers on the rings -
 * needs rings that exist, and these are them.
 */
int nss_wifili_start(struct seq_file *s)
{
	struct nss_wifili_ctx *w;
	struct nss_wifili_msg *m;
	struct nss_core *core;
	int i, ret;

	guard(mutex)(&nss_wifili_lock);

	core = nss_wifili_core;
	if (!core || !core->running)
		return -ENODEV;

	if (!nss_wifili_have_soc || !nss_wifili_pdevs) {
		seq_puts(s, "no WLAN driver has described its rings\n");
		return 0;
	}

	/* One initialisation per core, whichever asks for it: an accepted one
	 * leaves the radio in a state the next is judged against.
	 */
	if (core->wifili_probed) {
		seq_puts(s, "wifili is already initialised on this core\n");
		return 0;
	}
	core->wifili_probed = true;

	m = kzalloc(sizeof(*m), GFP_KERNEL);
	w = kzalloc(sizeof(*w), GFP_KERNEL);
	if (!m || !w) {
		ret = -ENOMEM;
		goto out;
	}

	/* The transmit descriptors are the firmware's to carve up and the
	 * host's to provide, so they are not part of what the WLAN driver
	 * describes.
	 */
	ret = nss_wifili_mem_get(core, &w->txdesc, NSS_WIFILI_TX_DESC_MEM);
	if (ret)
		goto out;

	ret = nss_wifili_mem_get(core, &w->txdescext, NSS_WIFILI_TX_DESC_MEM);
	if (ret)
		goto out;

	m->cm.interface = NSS_INTERFACE_WIFILI;
	m->cm.type = NSS_WIFILI_INIT_MSG;
	m->msg.init.hssm.dev_base_addr = nss_wifili_soc.dev_base;
	m->msg.init.hssm.shadow_rdptr_mem_addr = nss_wifili_soc.shadow_rd;
	m->msg.init.hssm.shadow_wrptr_mem_addr = nss_wifili_soc.shadow_wr;
	m->msg.init.hssm.lmac_rings_start_id = nss_wifili_soc.lmac_ring_start;
	m->msg.init.num_tcl_data_rings = nss_wifili_soc.num_tcl;
	m->msg.init.num_reo_dest_rings = nss_wifili_soc.num_reo;

	for (i = 0; i < nss_wifili_soc.num_tcl; i++) {
		nss_wifili_ring_fill(&m->msg.init.tcl_ring_info[i],
				     &nss_wifili_soc.tcl[i]);
		nss_wifili_ring_fill(&m->msg.init.tx_comp_ring[i],
				     &nss_wifili_soc.txcomp[i]);
	}

	for (i = 0; i < nss_wifili_soc.num_reo; i++)
		nss_wifili_ring_fill(&m->msg.init.reo_dest_ring[i],
				     &nss_wifili_soc.reo_dest[i]);

	nss_wifili_ring_fill(&m->msg.init.reo_reinject_ring,
			     &nss_wifili_soc.reo_reinject);
	nss_wifili_ring_fill(&m->msg.init.rx_rel_ring, &nss_wifili_soc.rx_rel);
	nss_wifili_ring_fill(&m->msg.init.reo_exception_ring,
			     &nss_wifili_soc.reo_exception);

	/* One descriptor pool per radio. The message carries a separate count
	 * for each, and the firmware reaps a pool per radio whatever the count
	 * says, so a pool left at zero is one it reaps against and never
	 * sized.
	 */
	m->msg.init.wtdim.num_pool = nss_wifili_pdevs;
	m->msg.init.wtdim.num_tx_desc = NSS_WIFILI_TX_DESC;
	m->msg.init.wtdim.num_tx_desc_ext = NSS_WIFILI_TX_DESC;
	if (nss_wifili_pdevs > 1) {
		m->msg.init.wtdim.num_tx_desc_2 = NSS_WIFILI_TX_DESC;
		m->msg.init.wtdim.num_tx_desc_ext_2 = NSS_WIFILI_TX_DESC;
	}
	if (nss_wifili_pdevs > 2) {
		m->msg.init.wtdim.num_tx_desc_3 = NSS_WIFILI_TX_DESC;
		m->msg.init.wtdim.num_tx_desc_ext_3 = NSS_WIFILI_TX_DESC;
	}

	m->msg.init.wtdim.num_memaddr = 2;
	m->msg.init.wtdim.memory_addr[0] = w->txdesc.dma;
	m->msg.init.wtdim.memory_size[0] = w->txdesc.size;
	m->msg.init.wtdim.memory_addr[1] = w->txdescext.dma;
	m->msg.init.wtdim.memory_size[1] = w->txdescext.size;
	m->msg.init.wtdim.ext_desc_page_num = 1;
	m->msg.init.wtdim.num_tx_device_limit = NSS_WIFILI_TX_DESC *
						nss_wifili_pdevs;

	m->msg.init.target_type = nss_wifili_soc.target_type;
	m->msg.init.wrip.tlv_size = nss_wifili_soc.tlv_size;
	m->msg.init.wrip.rx_buf_len = nss_wifili_soc.rx_buf_len;

	/* A fault in the firmware's transmit path names a descriptor by a page
	 * and an offset within it, both of which are the host's. Print what was
	 * handed over so a trapped address can be told apart from a stray one.
	 */
	dev_info(core->dev,
		 "wifili: txdesc %pad+%zx ext %pad+%zx, %u pools, %u tcl, %u reo\n",
		 &w->txdesc.dma, w->txdesc.size, &w->txdescext.dma,
		 w->txdescext.size, nss_wifili_pdevs, nss_wifili_soc.num_tcl,
		 nss_wifili_soc.num_reo);
	for (i = 0; i < nss_wifili_soc.num_tcl; i++)
		dev_info(core->dev,
			 "wifili: tcl%d id %u base %#x x%u  txcomp id %u base %#x x%u hw %#x/%#x\n",
			 i, nss_wifili_soc.tcl[i].id,
			 nss_wifili_soc.tcl[i].base,
			 nss_wifili_soc.tcl[i].num_entries,
			 nss_wifili_soc.txcomp[i].id,
			 nss_wifili_soc.txcomp[i].base,
			 nss_wifili_soc.txcomp[i].num_entries,
			 nss_wifili_soc.txcomp[i].hwreg[0],
			 nss_wifili_soc.txcomp[i].hwreg[1]);

	ret = nss_msg_send(core, m, offsetof(struct nss_wifili_msg, msg) +
				    sizeof(m->msg.init));
	seq_printf(s, "%-14s rc %d response %u error %u\n", "soc init:", ret,
		   m->cm.response, m->cm.error);
	if (ret)
		goto out;

	for (i = 0; i < nss_wifili_pdevs; i++) {
		int if_num;

		/* A radio is a node before it is a radio. The initialisation
		 * that follows is addressed to the SoC but names the radio,
		 * and the firmware has nothing to name until one exists.
		 */
		if_num = nss_wifili_radio_alloc(core, m);
		nss_wifili_radio_if[i] = if_num;
		seq_printf(s, "radio %u node:  if_num %d\n",
			   nss_wifili_pdev[i].radio_id, if_num);
		if (if_num < 0) {
			ret = if_num;
			goto out;
		}

		memset(m, 0, sizeof(*m));
		m->cm.interface = NSS_INTERFACE_WIFILI;
		m->cm.type = NSS_WIFILI_PDEV_INIT_MSG;
		nss_wifili_ring_fill(&m->msg.pdevmsg.rxdma_ring,
				     &nss_wifili_pdev[i].rxdma);
		m->msg.pdevmsg.radio_id = nss_wifili_pdev[i].radio_id;
		m->msg.pdevmsg.lmac_id = nss_wifili_pdev[i].lmac_id;
		m->msg.pdevmsg.target_pdev_id = nss_wifili_pdev[i].target_pdev_id;
		m->msg.pdevmsg.num_rx_swdesc = nss_wifili_pdev[i].num_rx_swdesc;
		ret = nss_msg_send(core, m,
				   offsetof(struct nss_wifili_msg, msg) +
				   sizeof(m->msg.pdevmsg));
		seq_printf(s, "radio %u init:  rc %d response %u error %u\n",
			   nss_wifili_pdev[i].radio_id, ret, m->cm.response,
			   m->cm.error);
		if (ret)
			goto out;
	}

	memset(m, 0, sizeof(*m));
	m->cm.interface = NSS_INTERFACE_WIFILI;
	m->cm.type = NSS_WIFILI_START_MSG;
	ret = nss_msg_send(core, m, offsetof(struct nss_wifili_msg, msg));
	seq_printf(s, "%-14s rc %d response %u error %u\n", "start:", ret,
		   m->cm.response, m->cm.error);
	if (!ret)
		nss_wifili_vdev_probe(core, s, nss_wifili_radio_if[0]);
out:
	kfree(w);
	kfree(m);

	return 0;
}
