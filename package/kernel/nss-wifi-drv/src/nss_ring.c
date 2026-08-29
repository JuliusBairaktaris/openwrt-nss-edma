// SPDX-License-Identifier: GPL-2.0-only
/* The message and data rings between the host and a running core.
 *
 * The rings live in coherent memory, so the indices each side publishes are
 * visible to the other without maintenance of our own. Each of the ten
 * interrupt causes is its own line - this SoC has no mask or status register
 * for them - so a cause is fixed when its line is claimed and never has to be
 * decoded.
 *
 * A running core's first act is to ask for buffers, because it has none. That
 * request is also how the host learns it is up: there is no ready message in
 * this protocol.
 */

#include <linux/cleanup.h>
#include <linux/interrupt.h>
#include <linux/of_irq.h>
#include <linux/etherdevice.h>
#include <net/dsa.h>
#include <linux/platform_device.h>
#include <linux/seq_file.h>
#include <linux/soc/qcom/qca_edma.h>
#include <linux/spinlock.h>

#include "nss_drv.h"

#define NSS_NAPI_WEIGHT		64
#define NSS_REFILL_BATCH	64

void nss_doorbell(struct nss_core *core, u32 which)
{
	writel(BIT(NSS_H2N_INTR_BASE(core->id) + which),
	       core->qgic + NSS_QGIC_IPC_REG);
}

/* Give the firmware's interface numbers something to deliver to.
 *
 * A frame the firmware exceptions carries the number of the interface it came
 * in on, and for a switch port that number is the port's own index - measured
 * on this SoC, and derived here rather than written down, because the port set
 * is a property of the board. Everything above a switch port gets its number
 * when it is created, and registers itself.
 */
void nss_iface_bind(struct nss_core *core)
{
	struct net_device *dev;

	rtnl_lock();
	for_each_netdev(&init_net, dev) {
		struct dsa_port *dp;

		if (!dsa_user_dev_check(dev))
			continue;

		dp = dsa_port_from_netdev(dev);
		if (IS_ERR(dp) || dp->index >= NSS_INTERFACE_MAX)
			continue;

		if (dsa_port_to_conduit(dp) != core->conduit)
			continue;

		dev_hold(dev);
		rcu_assign_pointer(core->iface[dp->index], dev);
	}
	rtnl_unlock();
}

void nss_iface_unbind(struct nss_core *core)
{
	int i;

	for (i = 0; i < NSS_INTERFACE_MAX; i++) {
		struct net_device *dev;

		dev = rcu_replace_pointer(core->iface[i], NULL, true);
		if (dev)
			dev_put(dev);
	}

	synchronize_rcu();
}

/* Hand up a frame the firmware exceptioned.
 *
 * The offset and length come from the other side, so they are checked against
 * the allocation the host made rather than believed: the buffer is the one
 * this driver donated, and nothing the firmware writes into the descriptor can
 * make it larger.
 */
static bool nss_data_recv(struct nss_core *core, struct napi_struct *napi,
			  const struct n2h_descriptor *desc,
			  struct sk_buff *skb)
{
	u32 end = (u32)desc->payload_offs + desc->payload_len;
	struct net_device *dev;

	if (desc->buffer_type != NSS_N2H_BUFFER_PACKET)
		return false;

	dev = rcu_dereference(core->iface[desc->interface_num]);
	if (!dev || end > NSS_EMPTY_BUFFER_ALLOC || desc->payload_len < ETH_HLEN)
		return false;

	dma_unmap_single(core->dev, NSS_SKB_CB(skb)->dma,
			 NSS_EMPTY_BUFFER_ALLOC, DMA_FROM_DEVICE);

	skb_reserve(skb, desc->payload_offs);
	skb_put(skb, desc->payload_len);
	skb->protocol = eth_type_trans(skb, dev);

	dev_sw_netstats_rx_add(dev, desc->payload_len);
	napi_gro_receive(napi, skb);

	return true;
}

/* Hand on a message the firmware sent unasked.
 *
 * Nothing the host sent is outstanding, so this is never a completion and the
 * sixteen header bytes reserved for the host hold whatever the buffer's last
 * user left there. The descriptor's interface number is zero on every message
 * buffer, so the dispatch is on the interface inside the payload, whose top
 * byte is a core id. The buffer is already unmapped, so the payload is the
 * host's to read.
 */
static void nss_notify_recv(struct nss_core *core,
			    const struct n2h_descriptor *desc,
			    struct sk_buff *skb)
{
	u32 end = (u32)desc->payload_offs + desc->payload_len;
	const struct nss_cmn_msg *ncm;

	if (desc->buffer_type != NSS_N2H_BUFFER_STATUS ||
	    end > NSS_EMPTY_BUFFER_ALLOC || desc->payload_len < sizeof(*ncm))
		return;

	core->notify++;

	ncm = (const void *)(skb->head + desc->payload_offs);
	if (NSS_INTERFACE_NUM_GET(ncm->interface) == NSS_INTERFACE_WIFILI)
		nss_wifili_notify(core, ncm, desc->payload_len);
}

/* Take the switch's CPU port back off the firmware.
 *
 * The firmware programs the queue-to-ring table as part of its own EDMA
 * bring-up, which happens after it asks for the buffers that tell the host it
 * is running, and it publishes no moment at which it has finished. A frame
 * arriving on a data queue is that moment: the only way one gets there is
 * through a table entry naming a firmware ring. So the first frame the
 * firmware takes from the host is what buys the port back, and every frame
 * after it stays with the host.
 */
static void nss_cpu_port_reclaim(struct nss_core *core)
{
	if (!core->cpu_port_taken || core->cpu_port_to_fw)
		return;

	core->cpu_port_taken = false;
	qca_edma_cpu_queues_to_host(core->conduit);
	dev_info(core->dev, "CPU-port queues returned to the host\n");
}

/* Keep every line the firmware writes, not just the last ringful.
 *
 * The block the firmware asks for at boot fits thirty-two entries and a fault
 * fills far more than that, so by the time the host is told a fault happened
 * the beginning of it - the part that says what went wrong - has been
 * overwritten by the lines that follow it.
 *
 * So the host copies entries out as they appear, appending anything new to a
 * buffer big enough for a whole fault. A line is lost if it arrives faster
 * than the sampling or after the buffer is full, and the readout says how
 * many rather than hiding it.
 */
#define NSS_LOG_SHADOW_ENTRIES	1024
#define NSS_LOG_SHADOW_POLL_US	200

/* Runs from a timer and from whoever is reading, so it is serialised: two
 * appenders racing on the same index would write past the buffer.
 */
static void nss_log_shadow_take(struct nss_core *core)
{
	u32 n = core->log_entries;
	u32 written, first, i;

	if (!core->log || !core->shadow || !n)
		return;

	guard(spinlock_irqsave)(&core->shadow_lock);

	written = READ_ONCE(core->log->current_entry);
	if (written == core->shadow_seen)
		return;

	/* Anything more than a ring behind is already gone. */
	first = max(core->shadow_seen, written > n ? written - n : 0);

	for (i = first; i < written; i++) {
		struct nss_log_entry *src = &core->log->log_ring_buffer[i % n];

		if (core->shadow_held >= NSS_LOG_SHADOW_ENTRIES)
			break;

		core->shadow[core->shadow_held++] = *src;
	}

	core->shadow_seen = written;
}

static enum hrtimer_restart nss_log_shadow_tick(struct hrtimer *t)
{
	struct nss_core *core = container_of(t, struct nss_core, shadow_timer);

	nss_log_shadow_take(core);
	hrtimer_forward_now(t, us_to_ktime(NSS_LOG_SHADOW_POLL_US));

	return HRTIMER_RESTART;
}

int nss_log_shadow_init(struct nss_core *core)
{
	core->shadow = devm_kcalloc(core->dev, NSS_LOG_SHADOW_ENTRIES,
				    sizeof(*core->shadow), GFP_KERNEL);
	if (!core->shadow)
		return -ENOMEM;

	spin_lock_init(&core->shadow_lock);
	hrtimer_setup(&core->shadow_timer, nss_log_shadow_tick, CLOCK_MONOTONIC,
		      HRTIMER_MODE_REL);

	return 0;
}

void nss_log_shadow_start(struct nss_core *core)
{
	scoped_guard(spinlock_irqsave, &core->shadow_lock) {
		core->shadow_seen = 0;
		core->shadow_held = 0;
	}

	hrtimer_start(&core->shadow_timer, us_to_ktime(NSS_LOG_SHADOW_POLL_US),
		      HRTIMER_MODE_REL);
}

void nss_log_shadow_stop(struct nss_core *core)
{
	hrtimer_cancel(&core->shadow_timer);
	nss_log_shadow_take(core);
}

/* Walk what the firmware has been saying, oldest first.
 *
 * The write position is a free-running count of lines ever written, not an
 * index, so how many are valid follows from it rather than from the ring
 * being full. The host zeroed the block, so a slot whose cookie does not read
 * back as NSS_LOG_COOKIE has not been written and is skipped.
 */
static void nss_log_walk(struct nss_core *core, struct seq_file *s)
{
	u32 n = core->log_entries;
	u32 written, count, i;

	if (!core->log || !n)
		return;

	nss_log_shadow_take(core);

	written = READ_ONCE(core->log->current_entry);
	scoped_guard(spinlock_irqsave, &core->shadow_lock)
		count = core->shadow_held;
	if (written > count && s)
		seq_printf(s, "[%u lines arrived faster than they could be kept]\n",
			   written - count);

	for (i = 0; i < count; i++) {
		struct nss_log_entry *e = &core->shadow[i];

		if (e->cookie != NSS_LOG_COOKIE)
			continue;

		if (s)
			seq_printf(s, "[%llu] %.*s\n", e->sequence_num,
				   NSS_LOG_LINE_WIDTH, e->message);
		else
			dev_err(core->dev, "  fw[%llu] %.*s\n", e->sequence_num,
				NSS_LOG_LINE_WIDTH, e->message);
	}
}

int nss_log_show_ring(struct nss_core *core, struct seq_file *s)
{
	if (!core->log || !core->log_entries)
		return -ENODEV;

	nss_log_walk(core, s);

	return 0;
}

/* The only account of a core that has stopped, so it is dumped whenever one
 * does. The ring has been empty on every start observed here and has carried
 * entries only after a trap, so an empty ring is a result and not a failed
 * read: the core stopped without trapping.
 */
void nss_log_dump(struct nss_core *core, const char *why)
{
	dev_err(core->dev, "%s: firmware log follows\n", why);
	nss_log_walk(core, NULL);
}

/* Hand the firmware empty buffers. It picks its own offset inside each one
 * and reports where the payload landed, so what it is given is the head of
 * the allocation rather than the data pointer.
 */
static int nss_refill(struct nss_core *core, int budget)
{
	struct nss_h2n_ring *ring = &core->h2n[NSS_H2N_RING_EMPTY_BUF];
	struct nss_if_mem_map *map = core->if_map;
	u32 size = NSS_EMPTY_BUFFER_ALLOC;
	int filled = 0;

	guard(spinlock_bh)(&ring->lock);

	while (filled < budget) {
		u32 next = (ring->hlos_index + 1) & (NSS_RING_ENTRIES - 1);
		struct h2n_descriptor *desc;
		struct sk_buff *skb;
		dma_addr_t dma;

		if (next == READ_ONCE(map->h2n_nss_index[NSS_H2N_RING_EMPTY_BUF]))
			break;

		/* Custody of these buffers passes to the firmware for as long
		 * as it likes, so they are allocated outright rather than
		 * carved out of the per-CPU fragment cache, where each one in
		 * flight would pin the whole page it came from.
		 */
		skb = alloc_skb(size, GFP_ATOMIC);
		if (!skb)
			break;

		dma = dma_map_single(core->dev, skb->head, size,
				     DMA_FROM_DEVICE);
		if (dma_mapping_error(core->dev, dma)) {
			kfree_skb(skb);
			break;
		}

		NSS_SKB_CB(skb)->dma = dma;

		desc = &ring->desc[ring->hlos_index];
		desc->buffer = dma;
		desc->buffer_len = size;
		desc->payload_len = 0;
		desc->payload_offs = 0;
		desc->buffer_type = NSS_H2N_BUFFER_EMPTY;
		desc->bit_flags = 0;
		desc->interface_num = 0;
		desc->opaque = (uintptr_t)skb;

		ring->hlos_index = next;
		filled++;
	}

	if (filled) {
		WRITE_ONCE(map->h2n_hlos_index[NSS_H2N_RING_EMPTY_BUF],
			   ring->hlos_index);
		atomic_add(filled, &core->buffers_queued);
		nss_doorbell(core, NSS_H2N_INTR_EMPTY_BUF);
	}

	return filled;
}

/* The firmware is up the first time it asks for buffers. Before believing it,
 * check that the page both sides share carries the value the firmware writes
 * once it has taken it over.
 */
static void nss_check_booted(struct nss_core *core)
{
	if (core->running)
		return;

	if (core->if_map->magic != NSS_IF_MEM_MAP_MAGIC) {
		dev_err(core->dev, "core %u answered with magic %#x\n",
			core->id, core->if_map->magic);
		return;
	}

	core->running = true;
	complete(&core->booted);
	dev_info(core->dev, "core %u booted\n", core->id);
}

static int nss_poll_sos(struct napi_struct *napi, int budget)
{
	struct nss_irq_ctx *ctx = container_of(napi, struct nss_irq_ctx, napi);
	struct nss_core *core = ctx->core;

	nss_check_booted(core);
	nss_refill(core, min(budget, NSS_REFILL_BATCH));

	napi_complete(napi);
	enable_irq(ctx->irq);

	return 0;
}

/* Take back whatever the firmware has returned. A buffer comes back either as
 * a packet on a data queue or as an unused one on the return queue; both are
 * released here, because nothing above this driver consumes them yet.
 */
static int nss_poll_n2h(struct napi_struct *napi, int budget)
{
	struct nss_irq_ctx *ctx = container_of(napi, struct nss_irq_ctx, napi);
	struct nss_core *core = ctx->core;
	struct nss_n2h_ring *ring;
	struct nss_if_mem_map *map;
	u32 nss_index, count;
	int returned = 0;
	int done = 0;
	int qid;

	/* Ring 0 carries buffers the firmware is giving back unused; rings 1
	 * to 4 are the four data queues.
	 */
	if (ctx->cause == NSS_CAUSE_EMPTY_BUFFER_QUEUE)
		qid = NSS_N2H_RING_EMPTY_BUF;
	else
		qid = ctx->cause - NSS_CAUSE_DATA_QUEUE_0 + 1;

	ring = &core->n2h[qid];
	map = core->if_map;

	nss_index = READ_ONCE(map->n2h_nss_index[qid]);
	count = (nss_index - ring->hlos_index) & (NSS_RING_ENTRIES - 1);
	count = min_t(u32, count, budget);

	while (done < count) {
		struct n2h_descriptor *desc = &ring->desc[ring->hlos_index];
		struct sk_buff *skb = (struct sk_buff *)(uintptr_t)desc->opaque;

		/* The message buffer comes back on this ring like everything
		 * else, and is the host's own: it is answered rather than
		 * freed, and it was never one of the donated buffers.
		 */
		core->rx_type[desc->buffer_type & 7]++;
		if (desc->interface_num < NSS_INTERFACE_MAX)
			core->rx_iface[desc->interface_num]++;

		if (nss_msg_complete(core, desc)) {
			/* nothing to release */
		} else if (skb && nss_data_recv(core, napi, desc, skb)) {
			atomic_dec(&core->buffers_queued);
			returned++;
		} else if (skb) {
			dma_unmap_single(core->dev, NSS_SKB_CB(skb)->dma,
					 NSS_EMPTY_BUFFER_ALLOC, DMA_FROM_DEVICE);
			nss_notify_recv(core, desc, skb);
			dev_kfree_skb_any(skb);
			atomic_dec(&core->buffers_queued);
			returned++;
		}

		ring->hlos_index = (ring->hlos_index + 1) &
				   (NSS_RING_ENTRIES - 1);
		done++;
	}

	if (done)
		WRITE_ONCE(map->n2h_hlos_index[qid], ring->hlos_index);

	/* A frame on a data queue is one the firmware took from the host. */
	if (returned && qid != NSS_N2H_RING_EMPTY_BUF)
		nss_cpu_port_reclaim(core);

	/* Whatever came back leaves the firmware that much shorter. */
	nss_refill(core, returned);

	if (done < budget) {
		napi_complete(napi);
		enable_irq(ctx->irq);
	}

	return done;
}

/* A core that has dumped is finished. Stock brings the whole box down here,
 * on the argument that the data plane is gone either way; this driver keeps
 * the box up, because the host stack is still perfectly able to route and the
 * firmware log is worth more read than lost to a panic.
 */
static int nss_poll_coredump(struct napi_struct *napi, int budget)
{
	struct nss_irq_ctx *ctx = container_of(napi, struct nss_irq_ctx, napi);
	struct nss_core *core = ctx->core;

	core->running = false;
	dev_err(core->dev, "core %u has crashed; leaving it stopped\n",
		core->id);
	nss_log_dump(core, "coredump");

	/* Whatever the dead core still holds of the CPU port is given back
	 * here rather than at detach. Keeping the box routing through a
	 * firmware fault is the whole reason this driver does not panic, and
	 * a host that cannot receive is not routing.
	 */
	core->cpu_port_taken = true;
	nss_cpu_port_reclaim(core);

	napi_complete(napi);

	/* The line is deliberately left masked: the core is not coming back
	 * without a reset, and re-arming would only invite the same interrupt
	 * again.
	 */
	return 0;
}

static irqreturn_t nss_isr(int irq, void *data)
{
	struct nss_irq_ctx *ctx = data;

	disable_irq_nosync(irq);
	napi_schedule(&ctx->napi);

	return IRQ_HANDLED;
}

typedef int (*nss_poll_t)(struct napi_struct *napi, int budget);

static nss_poll_t nss_poll_fn(enum nss_cause cause)
{
	switch (cause) {
	case NSS_CAUSE_EMPTY_BUFFER_SOS:
		return nss_poll_sos;
	case NSS_CAUSE_EMPTY_BUFFER_QUEUE:
	case NSS_CAUSE_DATA_QUEUE_0:
	case NSS_CAUSE_DATA_QUEUE_1:
	case NSS_CAUSE_DATA_QUEUE_2:
	case NSS_CAUSE_DATA_QUEUE_3:
		return nss_poll_n2h;
	case NSS_CAUSE_COREDUMP_COMPLETE:
		return nss_poll_coredump;
	default:
		/* Paged buffers, transmit unblocking and the profiler are not
		 * used by this driver, so their lines are left unclaimed:
		 * they are edge triggered, and nothing here would act on
		 * them.
		 */
		return NULL;
	}
}

int nss_rings_start(struct nss_core *core)
{
	struct platform_device *pdev = to_platform_device(core->dev);
	int i, ret;

	core->ndev = alloc_netdev_dummy(0);
	if (!core->ndev)
		return -ENOMEM;

	for (i = 0; i < NSS_CAUSE_MAX; i++) {
		struct nss_irq_ctx *ctx = &core->irq[i];
		nss_poll_t poll = nss_poll_fn(i);

		ctx->core = core;
		ctx->cause = i;

		if (!poll)
			continue;

		ctx->irq = platform_get_irq(pdev, i);
		if (ctx->irq < 0) {
			ret = ctx->irq;
			goto err;
		}

		netif_napi_add_weight(core->ndev, &ctx->napi, poll,
				      NSS_NAPI_WEIGHT);
		napi_enable(&ctx->napi);
		ctx->napi_added = true;

		ret = request_irq(ctx->irq, nss_isr, 0, dev_name(core->dev),
				  ctx);
		if (ret)
			goto err;
	}

	return 0;

err:
	nss_rings_stop(core);

	return ret;
}

void nss_rings_stop(struct nss_core *core)
{
	int i;

	for (i = 0; i < NSS_CAUSE_MAX; i++) {
		struct nss_irq_ctx *ctx = &core->irq[i];

		if (!ctx->napi_added)
			continue;

		/* Free the line before stopping the poll: an interrupt that
		 * arrived while this ran would otherwise schedule a NAPI
		 * instance that is about to be deleted.
		 */
		free_irq(ctx->irq, ctx);
		napi_disable(&ctx->napi);
		netif_napi_del(&ctx->napi);
		ctx->napi_added = false;
	}

	if (core->ndev) {
		free_netdev(core->ndev);
		core->ndev = NULL;
	}
}
