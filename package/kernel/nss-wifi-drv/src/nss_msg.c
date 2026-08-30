// SPDX-License-Identifier: GPL-2.0-only
/* Asking the firmware to do something, and hearing back.
 *
 * A control message goes out on its own host-to-firmware ring, which the
 * firmware drains ahead of every data ring. The firmware writes its answer
 * into the same buffer and hands that buffer straight back, so a message is a
 * single block that travels out and returns rather than a request and a
 * separate reply.
 *
 * Two things about that are not obvious and are both load-bearing. The
 * descriptor's payload length is not the size of the request: it is the room
 * the firmware is given for its answer, and it is also the bound the firmware
 * checks the message length against. And the first and last segment flags are
 * not optional on a single-segment buffer - without the first, the firmware
 * takes the scatter-gather path, finds no head, and drops the descriptor
 * without ever returning the buffer.
 */

#include <linux/cleanup.h>
#include <linux/completion.h>
#include <linux/dma-mapping.h>
#include <linux/mutex.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#include "nss_drv.h"
#include "nss_dynamic_interface.h"

/* The firmware answers in the buffer it was sent, so the buffer is as large
 * as the largest answer rather than as the request.
 */
#define NSS_MSG_BUF_SIZE	NSS_EMPTY_BUFFER_SIZE

#define NSS_MSG_TIMEOUT_MS	2000

int nss_msg_init(struct nss_core *core)
{
	int ret;

	ret = devm_mutex_init(core->dev, &core->msg.lock);
	if (ret)
		return ret;

	init_completion(&core->msg.done);

	core->msg.buf = dmam_alloc_coherent(core->dev, NSS_MSG_BUF_SIZE,
					    &core->msg.dma, GFP_KERNEL);
	if (!core->msg.buf)
		return -ENOMEM;

	return 0;
}

/* True when the descriptor is carrying the message buffer back. The host
 * matches on the address it handed out, so a reply is recognised without
 * dereferencing anything the firmware wrote.
 */
bool nss_msg_complete(struct nss_core *core, const struct n2h_descriptor *desc)
{
	if (desc->buffer != core->msg.dma)
		return false;

	complete(&core->msg.done);

	return true;
}

/* Send a message and wait for the firmware's answer in place.
 *
 * One at a time: bring-up is a sequence of questions, and a second buffer
 * would need a way to tell two answers apart that the protocol does not
 * provide.
 */
/* What the firmware says when nothing asked it.
 *
 * Every unsolicited message is counted by type, and the first of each type
 * keeps its opening words. Two of them the host owes an answer to and the
 * shape of the answer is not derivable from the header: the peer memory
 * request names how much memory per station the running firmware wants for
 * its own peer record, which differs between firmware lines and is a
 * structure the host writes into blind. Reading it off the blob is the only
 * honest way to size that allocation.
 */
void nss_msg_seen(struct nss_core *core, const struct nss_cmn_msg *ncm,
		  u32 len)
{
	struct nss_msg_seen *seen;
	u32 words, i;

	if (ncm->type >= ARRAY_SIZE(core->seen))
		return;

	seen = &core->seen[ncm->type];
	words = min_t(u32, len / sizeof(u32), ARRAY_SIZE(seen->last));

	/* The first and the latest, because a counter that is pushed
	 * repeatedly says nothing in its first push: what is being asked of a
	 * statistics message is whether a number moved.
	 */
	for (i = 0; i < words; i++)
		seen->last[i] = ((const u32 *)ncm)[i];

	seen->lastwords = words;

	if (seen->count++)
		return;

	words = min_t(u32, words, ARRAY_SIZE(seen->word));
	for (i = 0; i < words; i++)
		seen->word[i] = ((const u32 *)ncm)[i];
	seen->words = words;
}

int nss_msg_send(struct nss_core *core, void *msg, size_t len)
{
	struct nss_cmn_msg *ncm = msg;
	struct nss_h2n_ring *ring = &core->h2n[NSS_H2N_RING_COMMAND];
	struct nss_if_mem_map *map = core->if_map;
	struct h2n_descriptor *desc;
	u32 next;
	int ret;

	if (len < sizeof(*ncm) || len > NSS_MSG_BUF_SIZE)
		return -EMSGSIZE;

	if (!core->running)
		return -ENODEV;

	guard(mutex)(&core->msg.lock);

	/* The header is the layer's to fill: the length the firmware checks
	 * excludes it, and getting that wrong is answered with a rejection
	 * rather than a hint.
	 */
	ncm->version = NSS_HLOS_MESSAGE_VERSION;
	ncm->len = len - sizeof(*ncm);
	memcpy(core->msg.buf, msg, len);

	scoped_guard(spinlock_bh, &ring->lock) {
		next = (ring->hlos_index + 1) & (NSS_RING_ENTRIES - 1);
		if (next == READ_ONCE(map->h2n_nss_index[NSS_H2N_RING_COMMAND]))
			return -EBUSY;

		desc = &ring->desc[ring->hlos_index];
		desc->interface_num = 0;
		desc->buffer = core->msg.dma;
		desc->buffer_len = NSS_MSG_BUF_SIZE;
		desc->payload_offs = 0;
		desc->payload_len = NSS_MSG_BUF_SIZE;
		desc->mss = 0;
		desc->qos_tag = 0;
		desc->buffer_type = NSS_H2N_BUFFER_CTRL;
		desc->bit_flags = NSS_H2N_FLAG_FIRST_SEGMENT |
				  NSS_H2N_FLAG_LAST_SEGMENT;
		desc->opaque = 0;

		ring->hlos_index = next;
		WRITE_ONCE(map->h2n_hlos_index[NSS_H2N_RING_COMMAND], next);
	}

	reinit_completion(&core->msg.done);
	nss_doorbell(core, NSS_H2N_INTR_DATA_CMD);

	if (!wait_for_completion_timeout(&core->msg.done,
					 msecs_to_jiffies(NSS_MSG_TIMEOUT_MS))) {
		dev_err(core->dev, "interface %u type %u: no answer in %u ms\n",
			ncm->interface, ncm->type, NSS_MSG_TIMEOUT_MS);
		return -ETIMEDOUT;
	}

	ncm = core->msg.buf;
	switch (ncm->response) {
	case NSS_CMN_RESPONSE_ACK:
		ret = 0;
		break;
	case NSS_CMN_RESPONSE_EMSG:
		dev_err(core->dev, "interface %u type %u refused: %u\n",
			ncm->interface, ncm->type, ncm->error);
		ret = -EPROTO;
		break;
	default:
		dev_err(core->dev, "interface %u type %u rejected: %u\n",
			ncm->interface, ncm->type, ncm->response);
		ret = -EPROTO;
		break;
	}

	memcpy(msg, core->msg.buf, len);

	return ret;
}

/* Whether the firmware has a node of this kind to give, and what it calls it.
 * A type it does not implement is refused at allocation, which is the cheapest
 * way to find out before a design leans on one.
 */
static void nss_alloc_probe(struct nss_core *core, struct seq_file *s,
			    enum nss_dynamic_interface_type type,
			    const char *what)
{
	struct nss_dynamic_interface_msg m;
	int if_num, ret;

	memset(&m, 0, sizeof(m));
	m.cm.interface = NSS_INTERFACE_DYNAMIC;
	m.cm.type = NSS_DYNAMIC_INTERFACE_ALLOC_NODE;
	m.msg.alloc_node.type = type;
	ret = nss_msg_send(core, &m, sizeof(m));
	if_num = m.msg.alloc_node.if_num;
	seq_printf(s, "%-14s rc %d response %u error %u if_num %d\n",
		   what, ret, m.cm.response, m.cm.error, if_num);
	if (ret)
		return;

	memset(&m, 0, sizeof(m));
	m.cm.interface = NSS_INTERFACE_DYNAMIC;
	m.cm.type = NSS_DYNAMIC_INTERFACE_DEALLOC_NODE;
	m.msg.dealloc_node.type = type;
	m.msg.dealloc_node.if_num = if_num;
	nss_msg_send(core, &m, sizeof(m));
}

/* Ask the firmware three questions whose answers can only come from it.
 *
 * A message layer with no consumer cannot be shown to work by reading it, and
 * the first real consumer is a whole milestone away. These three are the
 * smallest thing that distinguishes a working round trip from a plausible
 * one: a rejection the firmware can only produce by parsing the header, an
 * allocation whose answer is a number the host did not choose, and the
 * release that puts it back.
 */
int nss_msg_probe(struct nss_core *core, struct seq_file *s)
{
	struct nss_dynamic_interface_msg m;
	int if_num, ret;

	memset(&m, 0, sizeof(m));
	m.cm.interface = NSS_INTERFACE_DYNAMIC;
	m.cm.type = NSS_DYNAMIC_INTERFACE_MAX;
	ret = nss_msg_send(core, &m, sizeof(m));
	seq_printf(s, "unknown type:  rc %d response %u error %u\n",
		   ret, m.cm.response, m.cm.error);

	memset(&m, 0, sizeof(m));
	m.cm.interface = NSS_INTERFACE_DYNAMIC;
	m.cm.type = NSS_DYNAMIC_INTERFACE_ALLOC_NODE;
	m.msg.alloc_node.type = NSS_DYNAMIC_INTERFACE_TYPE_VLAN;
	ret = nss_msg_send(core, &m, sizeof(m));
	if_num = m.msg.alloc_node.if_num;
	seq_printf(s, "alloc vlan:    rc %d response %u if_num %d\n",
		   ret, m.cm.response, if_num);
	if (ret)
		return 0;

	memset(&m, 0, sizeof(m));
	m.cm.interface = NSS_INTERFACE_DYNAMIC;
	m.cm.type = NSS_DYNAMIC_INTERFACE_DEALLOC_NODE;
	m.msg.dealloc_node.type = NSS_DYNAMIC_INTERFACE_TYPE_VLAN;
	m.msg.dealloc_node.if_num = if_num;
	ret = nss_msg_send(core, &m, sizeof(m));
	seq_printf(s, "dealloc %d:    rc %d response %u error %u\n",
		   if_num, ret, m.cm.response, m.cm.error);

	nss_alloc_probe(core, s, NSS_DYNAMIC_INTERFACE_TYPE_GENERIC_REDIR_N2H,
			"redir n2h");
	nss_alloc_probe(core, s, NSS_DYNAMIC_INTERFACE_TYPE_GENERIC_REDIR_H2N,
			"redir h2n");

	return nss_wifili_probe(core, s);
}

/* What the firmware sends unasked, by interface and type.
 *
 * A message the host does not recognise is indistinguishable from one that was
 * never sent, and the two want opposite fixes. This counts every notification
 * by where it came from, so the difference is a readout rather than a guess.
 */
#define NSS_MSG_CENSUS_MAX	24

static struct {
	u32 interface;
	u32 type;
	u32 count;
} nss_msg_census_tbl[NSS_MSG_CENSUS_MAX];
static DEFINE_SPINLOCK(nss_msg_census_lock);

void nss_msg_census(const struct nss_cmn_msg *ncm)
{
	int i;

	guard(spinlock_bh)(&nss_msg_census_lock);

	for (i = 0; i < NSS_MSG_CENSUS_MAX; i++) {
		if (nss_msg_census_tbl[i].count &&
		    (nss_msg_census_tbl[i].interface != ncm->interface ||
		     nss_msg_census_tbl[i].type != ncm->type))
			continue;
		nss_msg_census_tbl[i].interface = ncm->interface;
		nss_msg_census_tbl[i].type = ncm->type;
		nss_msg_census_tbl[i].count++;
		return;
	}
}

void nss_msg_census_print(struct seq_file *s)
{
	int i;

	guard(spinlock_bh)(&nss_msg_census_lock);

	for (i = 0; i < NSS_MSG_CENSUS_MAX; i++)
		if (nss_msg_census_tbl[i].count)
			seq_printf(s, "interface %u type %u n %u\n",
				   nss_msg_census_tbl[i].interface,
				   nss_msg_census_tbl[i].type,
				   nss_msg_census_tbl[i].count);
}
