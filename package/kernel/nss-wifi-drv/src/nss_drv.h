/* SPDX-License-Identifier: GPL-2.0-only */
/* Shared state and the firmware's wire format for the NSS host driver.
 *
 * Everything named here that the firmware also reads or writes is fixed by the
 * blob and cannot be redesigned: the two descriptor layouts, the index page
 * they are described in, the message header, the meminfo request record and
 * the log ring. The rest is this driver's own.
 */

#ifndef __NSS_DRV_H
#define __NSS_DRV_H

#include <linux/clk.h>
#include <linux/dma-mapping.h>
#include <linux/hrtimer.h>
#include <linux/list.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <linux/seq_file.h>
#include <linux/types.h>

#include "nss_arch.h"
#include "nss_def.h"
#include "nss_cmn.h"

/* Ring geometry. The index page always describes 16 host-to-firmware and 15
 * firmware-to-host slots whatever the SoC populates, so the arrays are sized
 * by the page and the counts say how many are real.
 */
#define NSS_H2N_RING_SLOTS	16
#define NSS_N2H_RING_SLOTS	15
#define NSS_H2N_RINGS		11
#define NSS_N2H_RINGS		5
#define NSS_RING_ENTRIES	128

/* Each ring is allocated with two descriptors of slack that neither side ever
 * addresses. They set the stride between one ring's base and the next.
 */
#define NSS_RING_STRIDE_ENTRIES	(NSS_RING_ENTRIES + 2)

#define NSS_H2N_RING_EMPTY_BUF	0
#define NSS_H2N_RING_COMMAND	1
#define NSS_H2N_RING_DATA	3
#define NSS_N2H_RING_EMPTY_BUF	0

/* Buffer types, in the descriptors. */
#define NSS_H2N_BUFFER_EMPTY	0
#define NSS_H2N_BUFFER_PACKET	2
#define NSS_H2N_BUFFER_CTRL	4

#define NSS_N2H_BUFFER_EMPTY	1
#define NSS_N2H_BUFFER_PACKET	3
#define NSS_N2H_BUFFER_STATUS	6

#define NSS_H2N_FLAG_FIRST_SEGMENT	0x0004
#define NSS_H2N_FLAG_LAST_SEGMENT	0x0008

/* The firmware chooses its own offset inside a receive buffer, so the buffer
 * it is given is the head of the allocation and it reports where the payload
 * landed.
 */
#define NSS_EMPTY_BUFFER_SIZE	1984
#define NSS_EMPTY_BUFFER_ALLOC	(NSS_EMPTY_BUFFER_SIZE + NET_SKB_PAD)

/* Interface numbers the firmware dispatches on. */
#define NSS_INTERFACE_N2H	156
#define NSS_INTERFACE_ETH_RX	158
#define NSS_INTERFACE_DYNAMIC	176
#define NSS_INTERFACE_WIFILI	203
#define NSS_INTERFACE_MAX	228

/* Interrupt causes, one GIC line each: there are no mask or status registers
 * on this SoC, so the cause is fixed when the line is claimed.
 */
/* The order is the firmware's, not a choice: each entry is the index of the
 * interrupt line in the device tree, and the firmware decides what it raises
 * on which.
 */
enum nss_cause {
	NSS_CAUSE_EMPTY_BUFFER_SOS = 0,
	NSS_CAUSE_EMPTY_BUFFER_QUEUE = 1,
	NSS_CAUSE_TX_UNBLOCKED = 2,
	NSS_CAUSE_DATA_QUEUE_0 = 3,
	NSS_CAUSE_DATA_QUEUE_1 = 4,
	NSS_CAUSE_DATA_QUEUE_2 = 5,
	NSS_CAUSE_DATA_QUEUE_3 = 6,
	NSS_CAUSE_COREDUMP_COMPLETE = 7,
	NSS_CAUSE_PAGED_EMPTY_BUFFER_SOS = 8,
	NSS_CAUSE_PROFILE_DMA = 9,
	NSS_CAUSE_MAX,
};

/* Doorbell: the interrupt-cause bit for a host-to-firmware queue, written to
 * the shared QGIC register. Core 0's causes start at bit 13.
 */
#define NSS_QGIC_IPC_REG	0x8
#define NSS_H2N_INTR_BASE(id)	(13 + (id) * 6)
#define NSS_H2N_INTR_EMPTY_BUF	0
#define NSS_H2N_INTR_DATA_CMD	1

struct h2n_descriptor {
	u32 interface_num;
	u32 buffer;
	u32 qos_tag;
	u16 buffer_len;
	u16 payload_len;
	u16 mss;
	u16 payload_offs;
	u16 bit_flags;
	u8 buffer_type;
	u8 reserved;
	u64 opaque;
};

struct n2h_descriptor {
	u32 interface_num;
	u32 buffer;
	u16 buffer_len;
	u16 payload_len;
	u16 payload_offs;
	u16 bit_flags;
	u8 buffer_type;
	u8 response_type;
	u8 pri;
	u8 service_code;
	u32 reserved;
	u64 opaque;
};

static_assert(sizeof(struct h2n_descriptor) == 32);
static_assert(sizeof(struct n2h_descriptor) == 32);

struct h2n_desc_if_meta {
	u32 desc_addr;
	u16 size;
	u16 pad;
};

struct n2h_desc_if_meta {
	u32 desc_addr;
	u16 size;
	u16 pad;
};

/* The page both sides use to find the rings and to publish how far each has
 * got through them. The firmware owns the nss_index arrays and the host owns
 * the hlos_index arrays; neither writes the other's. The page is coherent
 * memory, so the only thing a read of the other side's index needs is for the
 * compiler not to invent or cache it - READ_ONCE and WRITE_ONCE, not volatile
 * on the type.
 */
struct nss_if_mem_map {
	struct h2n_desc_if_meta h2n_desc_if[NSS_H2N_RING_SLOTS];
	struct n2h_desc_if_meta n2h_desc_if[NSS_N2H_RING_SLOTS];
	u32 magic;
	u16 if_version;
	u8 h2n_rings;
	u8 n2h_rings;
	u32 h2n_nss_index[NSS_H2N_RING_SLOTS];
	u32 n2h_nss_index[NSS_N2H_RING_SLOTS];
	u8 num_phys_ports;
	u8 reserved1[3];
	u32 h2n_hlos_index[NSS_H2N_RING_SLOTS];
	u32 n2h_hlos_index[NSS_N2H_RING_SLOTS];
	u32 reserved;
};

static_assert(sizeof(struct nss_if_mem_map) == 512);

#define NSS_IF_MEM_MAP_MAGIC	0x4e52522e
#define NSS_IF_MEM_MAP_VERSION	1

/* Meminfo. The image declares what it needs in a table the host walks,
 * writing back the address it allocated for each entry.
 */
#define NSS_MEMINFO_OFFSET		8
#define NSS_MEMINFO_MAGIC_RESERVE	0x9526
#define NSS_MEMINFO_MAGIC_MAP		0x9527
#define NSS_MEMINFO_MAGIC_REQUEST	0x9528
#define NSS_MEMINFO_MAGIC_END		0x9529

#define NSS_MEMINFO_BLOCK_NAME_LEN	48

enum nss_meminfo_memtype {
	NSS_MEMINFO_MEMTYPE_IMEM,
	NSS_MEMINFO_MEMTYPE_SDRAM,
	NSS_MEMINFO_MEMTYPE_UTCM_SHARED,
	NSS_MEMINFO_MEMTYPE_INFO,
};

/* Log ring. The firmware writes entries and advances its own index; the host
 * only ever reads.
 */
#define NSS_LOG_COOKIE		0xff785634
#define NSS_LOG_LINE_WIDTH	132

struct nss_log_entry {
	u64 sequence_num;
	u32 cookie;
	u32 thread_num;
	u32 timestamp;
	char message[NSS_LOG_LINE_WIDTH];
} __aligned(32);

struct nss_log_descriptor {
	u32 cookie;
	u32 log_nentries;
	u32 current_entry;
	u8 pad[20];
	struct nss_log_entry log_ring_buffer[];
} __aligned(32);

static_assert(sizeof(struct nss_log_entry) == 160);
static_assert(sizeof(struct nss_log_descriptor) == 32);

/* The message header is the exported ABI, shared with every wifili and vdev
 * message that embeds it. What the firmware stores at cb and app_data is
 * sixteen bytes it never reads: they come back exactly as they were sent,
 * which is the only thing tying a reply to its request - there is no
 * sequence number anywhere in the protocol.
 */
#define NSS_HLOS_MESSAGE_VERSION	1

static_assert(sizeof(struct nss_cmn_msg) == 40);
static_assert(offsetof(struct nss_cmn_msg, cb) == 24);

/* What the host knows about a buffer it lent the firmware. The descriptor a
 * buffer comes back in is written by the other side, so its address and
 * length describe what the firmware did with the buffer, not what the host
 * mapped - and unmapping by those is a partial unmap of a different range.
 */
struct nss_skb_cb {
	dma_addr_t dma;
	bool tx;	/* a frame handed down, not a buffer handed over */
};

#define NSS_SKB_CB(skb) ((struct nss_skb_cb *)(skb)->cb)

static_assert(sizeof(struct nss_skb_cb) <= sizeof_field(struct sk_buff, cb));

/* A host-to-firmware ring and the buffers in flight on it. */
struct nss_h2n_ring {
	struct h2n_descriptor *desc;
	spinlock_t lock;
	u32 hlos_index;
};

struct nss_n2h_ring {
	struct n2h_descriptor *desc;
	u32 hlos_index;
};

/* One message in flight at a time, in a block the host owns outright.
 *
 * The firmware writes its answer into the same buffer the request arrived
 * in and hands it back, so the buffer is read and written by both sides and
 * belongs in coherent memory rather than in a streaming mapping whose
 * direction would have to be wrong in one of the two phases. It is also what
 * identifies the reply: the descriptor comes back carrying the address the
 * host handed out, so nothing has to trust a pointer the firmware echoed.
 */
struct nss_msg {
	void *buf;
	dma_addr_t dma;
	struct mutex lock;
	struct completion done;
};

struct nss_core;

struct nss_irq_ctx {
	struct nss_core *core;
	struct napi_struct napi;
	enum nss_cause cause;
	int irq;
	bool napi_added;
};

/* One coherent block handed to the firmware, kept so it can be released. */
struct nss_alloc {
	struct list_head node;
	void *cpu;
	dma_addr_t dma;
	size_t size;
};

struct nss_clk_cfg {
	const char *name;
	unsigned long rate;
};

/* One unsolicited message type: how many arrived, and what the first one
 * said. Eight words reaches past the common header into the body, which is
 * where a request the host must answer carries its numbers, and the first
 * word is the interface the message came from.
 */
#define NSS_MSG_SEEN_MAX	64

struct nss_msg_seen {
	u64 count;
	u32 words;
	u32 word[8];
};

struct nss_core {
	struct device *dev;
	void __iomem *nphys;
	void __iomem *qgic;
	void __iomem *imem;
	void __iomem *misc_reset;
	resource_size_t imem_size;
	u32 imem_base;
	u32 imem_used;
	phys_addr_t load_addr;
	resource_size_t region_base;
	resource_size_t region_size;
	unsigned long core_rate;
	u32 id;
	bool loaded;
	bool running;
	bool cpu_port_taken;
	bool cpu_port_to_fw;
	bool wifili_probed;
	bool wifili_started;
	bool clocks_on;
	struct mutex lock;
	struct dentry *debugfs;

	struct list_head allocs;
	struct nss_if_mem_map *if_map;
	struct nss_log_descriptor *log;
	u32 log_entries;
	struct nss_log_entry *shadow;
	u32 shadow_seen;
	u32 shadow_held;
	struct hrtimer shadow_timer;
	spinlock_t shadow_lock;
	struct nss_h2n_ring h2n[NSS_H2N_RINGS];
	struct nss_n2h_ring n2h[NSS_N2H_RINGS];
	struct nss_irq_ctx irq[NSS_CAUSE_MAX];
	struct net_device *ndev;
	struct net_device *conduit;

	struct nss_msg msg;
	struct net_device __rcu *iface[NSS_INTERFACE_MAX];
	u64 rx_iface[NSS_INTERFACE_MAX];
	struct nss_msg_seen seen[NSS_MSG_SEEN_MAX];
	u64 rx_type[8];
	u64 notify;
	u64 tx_done;
	u64 link_desc_seen;
	u64 link_desc_returned;
	atomic_t buffers_queued;
	struct completion booted;

	struct clk_bulk_data clks[];
};

int nss_meminfo_init(struct nss_core *core);
void nss_mem_free_all(struct nss_core *core);
int nss_rings_start(struct nss_core *core);
void nss_iface_bind(struct nss_core *core);
void nss_iface_unbind(struct nss_core *core);
void nss_rings_stop(struct nss_core *core);
void nss_rings_quiesce(struct nss_core *core);
int nss_data_send(struct nss_core *core, struct sk_buff *skb, u32 if_num);
void nss_doorbell(struct nss_core *core, u32 intr);
int nss_msg_init(struct nss_core *core);
int nss_msg_send(struct nss_core *core, void *msg, size_t len);
bool nss_msg_complete(struct nss_core *core, const struct n2h_descriptor *desc);
int nss_msg_probe(struct nss_core *core, struct seq_file *s);
void nss_msg_seen(struct nss_core *core, const struct nss_cmn_msg *ncm,
		  u32 len);
int nss_wifili_probe(struct nss_core *core, struct seq_file *s);
int nss_wifili_start(struct seq_file *s);
int nss_wifili_tx(struct seq_file *s);
void nss_wifili_notify(struct nss_core *core, const struct nss_cmn_msg *ncm,
		       u32 len);
void nss_wifili_bind(struct nss_core *core);
int nss_log_show_ring(struct nss_core *core, struct seq_file *s);
void nss_log_dump(struct nss_core *core, const char *why);
int nss_log_shadow_init(struct nss_core *core);
void nss_log_shadow_start(struct nss_core *core);
void nss_log_shadow_stop(struct nss_core *core);

#endif /* __NSS_DRV_H */
