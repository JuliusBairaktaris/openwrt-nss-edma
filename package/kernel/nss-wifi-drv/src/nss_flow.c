// SPDX-License-Identifier: GPL-2.0-only
/* Giving the firmware a connection the kernel has finished routing.
 *
 * The kernel offers every flow it accepts into its own software fast path to
 * each hardware back end registered on the table, and counts how many took it;
 * a back end that refuses one leaves it for the others and for software. So
 * this is a second back end beside the switch's, and neither needs to know the
 * other exists: the switch declines a flow that did not arrive on one of its
 * ports, and this declines one that did not arrive on a wireless device.
 *
 * The kernel offers the two directions of a connection separately and the
 * firmware wants a single rule carrying both. The two offers arrive one after
 * the other for the same connection, so the first is kept and the second
 * completes it. That is not merely convenient: the address a frame leaves
 * with in one direction appears only in that direction's offer, and a rule
 * built with the wrong one is accepted, acknowledged, and quietly delivers
 * every packet to the wrong station.
 *
 * Everything else the rule needs that the offer does not carry comes from the
 * connection the flow belongs to, which holds both tuples before and after
 * translation and, for TCP, the window the firmware has to keep checking
 * against once the kernel stops seeing the packets.
 */

#include <linux/cleanup.h>
#include <linux/etherdevice.h>
#include <linux/mutex.h>
#include <linux/netdevice.h>
#include <linux/rhashtable.h>
#include <linux/soc/qcom/nss_wifi.h>
#include <net/flow_offload.h>
#include <net/netfilter/nf_conntrack.h>
#include <net/netfilter/nf_flow_table.h>
#include <net/pkt_cls.h>

#include "nss_drv.h"
#include "nss_ipv4.h"
#include "nss_ipv6.h"

/* Clamp the priority to the range the egress queues express; a higher value
 * selects no distinct queue. The shipped PPE flow back end bounds the same
 * tag the same way.
 */
#define NSS_FLOW_MAX_PRIORITY	15

/* All four VLAN slots say "no tag" before anything else is decided, the
 * same value the public host caller fills them with unconditionally.
 */
#define NSS_FLOW_VLAN_NONE	0xfff

/* What one direction of a connection contributes to the rule. */
struct nss_flow_dir {
	u8 mac[ETH_ALEN];	/* address the other direction delivers to */
	int ifnum;
	u32 mtu;
	bool seen;
};

struct nss_flow_entry {
	struct rhash_head node;
	struct rcu_head rcu;	/* the debugfs walker holds no lock */
	unsigned long flow;
	struct nss_flow_dir dir[FLOW_OFFLOAD_DIR_MAX];
	u32 priority;
	u8 protocol;
	bool v6;
	bool installed;
	u64 lastused;
	union {
		struct nss_ipv4_5tuple t4;
		struct nss_ipv6_5tuple t6;
	};
};

static const struct rhashtable_params nss_flow_ht_params = {
	.head_offset	= offsetof(struct nss_flow_entry, node),
	.key_offset	= offsetof(struct nss_flow_entry, flow),
	.key_len	= sizeof(unsigned long),
	.automatic_shrinking = true,
};

static DEFINE_MUTEX(nss_flow_lock);
static LIST_HEAD(nss_flow_blocks);
static struct rhashtable nss_flow_table;
static bool nss_flow_ready;
static struct nss_core *nss_flow_core;
/* Why a flow was not taken. A flow the firmware does not carry and a flow it
 * was never offered look the same from outside, and so do the several reasons
 * for the first, so each is counted where it is decided.
 */
enum nss_flow_reject {
	NSS_FLOW_REJECT_NOT_WIRELESS,
	NSS_FLOW_REJECT_KEY,
	NSS_FLOW_REJECT_INTERFACE,
	NSS_FLOW_REJECT_ACTION,
	NSS_FLOW_REJECT_NO_MAC,
	NSS_FLOW_REJECT_NO_CT,
	NSS_FLOW_REJECT_FIRMWARE,
	NSS_FLOW_REJECT_MAX,
};

static const char *const nss_flow_reject_name[] = {
	"not-wireless", "key", "interface", "action", "no-mac", "no-ct",
	"firmware",
};

static u32 nss_flow_pushed, nss_flow_bound, nss_flow_reject[NSS_FLOW_REJECT_MAX];
static int nss_flow_last_ifindex;

static int nss_flow_reject_at(enum nss_flow_reject why, int ret)
{
	nss_flow_reject[why]++;

	return ret;
}

void nss_flow_bind(struct nss_core *core)
{
	guard(mutex)(&nss_flow_lock);
	nss_flow_core = core;
}

/* The connection an offer belongs to.
 *
 * The kernel names a direction by the address of its tuple, and the tuple says
 * which direction it is, so the connection is the same tuple array walked back
 * to its first element.
 */
static struct flow_offload *nss_flow_of(unsigned long cookie)
{
	const struct flow_offload_tuple *t = (const void *)cookie;
	struct flow_offload_tuple_rhash *th;

	th = container_of(t, struct flow_offload_tuple_rhash, tuple);

	return container_of(th - t->dir, struct flow_offload, tuplehash[0]);
}

static enum flow_offload_tuple_dir nss_flow_dir_of(unsigned long cookie)
{
	const struct flow_offload_tuple *t = (const void *)cookie;

	return t->dir;
}

/* The firmware's number for a network device, and its maximum frame size.
 *
 * The driver already holds one device per interface it has given the
 * firmware - a switch port it armed, a virtual device it created for a
 * radio - so the number is where the device is found rather than anything
 * that has to be derived.
 */
static int nss_flow_ifnum(struct nss_core *core, int ifindex, u32 *mtu)
{
	int i;

	guard(rcu)();

	for (i = 0; i < NSS_INTERFACE_MAX; i++) {
		struct net_device *dev = rcu_dereference(core->iface[i]);

		if (!dev || dev->ifindex != ifindex)
			continue;
		*mtu = dev->mtu;
		return i;
	}

	return -ENODEV;
}

static bool nss_flow_is_wireless(int ifindex)
{
	struct net_device *dev;

	guard(rcu)();

	dev = dev_get_by_index_rcu(&init_net, ifindex);

	return dev && dev->ieee80211_ptr;
}

/* Everything the firmware is told about one direction, out of that
 * direction's own offer.
 */
static int nss_flow_take_dir(struct nss_core *core, struct nss_flow_entry *e,
			     enum flow_offload_tuple_dir dir,
			     struct flow_cls_offload *f)
{
	struct flow_rule *rule = flow_cls_offload_flow_rule(f);
	struct nss_flow_dir *d = &e->dir[dir];
	struct flow_match_meta meta;
	struct flow_action_entry *act;
	struct ethhdr eth = {};
	int i;

	if (!flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_META))
		return nss_flow_reject_at(NSS_FLOW_REJECT_KEY, -EOPNOTSUPP);

	flow_rule_match_meta(rule, &meta);
	d->ifnum = nss_flow_ifnum(core, meta.key->ingress_ifindex, &d->mtu);
	if (d->ifnum < 0) {
		nss_flow_last_ifindex = meta.key->ingress_ifindex;
		return nss_flow_reject_at(NSS_FLOW_REJECT_INTERFACE, d->ifnum);
	}

	flow_action_for_each(i, act, &rule->action) {
		switch (act->id) {
		case FLOW_ACTION_MANGLE:
			if (act->mangle.htype != FLOW_ACT_MANGLE_HDR_TYPE_ETH)
				break;
			/* The kernel rewrites the header as two words at
			 * fixed offsets, so the address is read back out of
			 * a header rather than out of the action.
			 */
			if (act->mangle.offset > sizeof(eth) - sizeof(u32))
				return nss_flow_reject_at(NSS_FLOW_REJECT_ACTION,
							  -EOPNOTSUPP);
			*(u32 *)((u8 *)&eth + act->mangle.offset) =
				(*(u32 *)((u8 *)&eth + act->mangle.offset) &
				 ~act->mangle.mask) | act->mangle.val;
			break;
		case FLOW_ACTION_PRIORITY:
			e->priority = min_t(u32, act->priority,
					    NSS_FLOW_MAX_PRIORITY);
			break;
		case FLOW_ACTION_REDIRECT:
		case FLOW_ACTION_CSUM:
			break;
		default:
			/* A tunnel, a VLAN or an encapsulation this rule has
			 * no field for. Leaving the flow in software is the
			 * only honest answer.
			 */
			dev_warn_ratelimited(core->dev,
					     "flow declined: action %d, ingress %d\n",
					     act->id,
					     meta.key->ingress_ifindex);
			return nss_flow_reject_at(NSS_FLOW_REJECT_ACTION,
						  -EOPNOTSUPP);
		}
	}

	if (!is_valid_ether_addr(eth.h_dest))
		return nss_flow_reject_at(NSS_FLOW_REJECT_NO_MAC, -EOPNOTSUPP);

	/* The address this direction delivers to is the one the firmware
	 * needs for the connection's other half.
	 */
	ether_addr_copy(d->mac, eth.h_dest);
	d->seen = true;

	return 0;
}

static void nss_flow_mac(u16 out[3], const u8 *mac)
{
	memcpy(out, mac, ETH_ALEN);
}

static int nss_flow_send_v4(struct nss_core *core, struct nss_flow_entry *e,
			    struct nf_conn *ct)
{
	const struct nf_conntrack_tuple *o, *r;
	struct nss_ipv4_rule_create_msg *c;
	struct nss_ipv4_msg *m __free(kfree) =
		kzalloc(sizeof(*m), GFP_KERNEL);
	int ret;

	if (!m)
		return -ENOMEM;

	o = &ct->tuplehash[IP_CT_DIR_ORIGINAL].tuple;
	r = &ct->tuplehash[IP_CT_DIR_REPLY].tuple;
	c = &m->msg.rule_create;

	/* The firmware matches on host-endian words. */
	c->tuple.protocol = e->protocol;
	c->tuple.flow_ip = ntohl(o->src.u3.ip);
	c->tuple.flow_ident = ntohs(o->src.u.all);
	c->tuple.return_ip = ntohl(o->dst.u3.ip);
	c->tuple.return_ident = ntohs(o->dst.u.all);

	/* Translation is what the connection's two tuples differ by: the
	 * reply's destination is where the flow direction is rewritten to,
	 * and the reply's source is where the return direction comes from.
	 */
	c->conn_rule.flow_ip_xlate = ntohl(r->dst.u3.ip);
	c->conn_rule.flow_ident_xlate = ntohs(r->dst.u.all);
	c->conn_rule.return_ip_xlate = ntohl(r->src.u3.ip);
	c->conn_rule.return_ident_xlate = ntohs(r->src.u.all);

	nss_flow_mac(c->conn_rule.flow_mac,
		     e->dir[FLOW_OFFLOAD_DIR_REPLY].mac);
	nss_flow_mac(c->conn_rule.return_mac,
		     e->dir[FLOW_OFFLOAD_DIR_ORIGINAL].mac);

	c->conn_rule.flow_interface_num =
		e->dir[FLOW_OFFLOAD_DIR_ORIGINAL].ifnum;
	c->conn_rule.return_interface_num =
		e->dir[FLOW_OFFLOAD_DIR_REPLY].ifnum;
	c->conn_rule.flow_mtu = e->dir[FLOW_OFFLOAD_DIR_ORIGINAL].mtu;
	c->conn_rule.return_mtu = e->dir[FLOW_OFFLOAD_DIR_REPLY].mtu;

	c->vlan_primary_rule.ingress_vlan_tag = NSS_FLOW_VLAN_NONE;
	c->vlan_primary_rule.egress_vlan_tag = NSS_FLOW_VLAN_NONE;
	c->vlan_secondary_rule.ingress_vlan_tag = NSS_FLOW_VLAN_NONE;
	c->vlan_secondary_rule.egress_vlan_tag = NSS_FLOW_VLAN_NONE;

	c->qos_rule.flow_qos_tag = e->priority;
	c->qos_rule.return_qos_tag = e->priority;

	/* Left zero the nexthop fields name interface 0 and the frames go
	 * there, so they repeat the connection rule's interfaces. The public
	 * host caller sets them the same way.
	 */
	c->nexthop_rule.flow_nexthop = c->conn_rule.flow_interface_num;
	c->nexthop_rule.return_nexthop = c->conn_rule.return_interface_num;

	c->rule_flags = NSS_IPV4_RULE_CREATE_FLAG_ROUTED;
	c->valid_flags = NSS_IPV4_RULE_CREATE_CONN_VALID |
			 NSS_IPV4_RULE_CREATE_QOS_VALID |
			 NSS_IPV4_RULE_CREATE_NEXTHOP_VALID;

	if (e->protocol == IPPROTO_TCP) {
		spin_lock_bh(&ct->lock);
		c->tcp_rule.flow_window_scale = ct->proto.tcp.seen[IP_CT_DIR_ORIGINAL].td_scale;
		c->tcp_rule.flow_max_window = ct->proto.tcp.seen[IP_CT_DIR_ORIGINAL].td_maxwin;
		c->tcp_rule.flow_end = ct->proto.tcp.seen[IP_CT_DIR_ORIGINAL].td_end;
		c->tcp_rule.flow_max_end = ct->proto.tcp.seen[IP_CT_DIR_ORIGINAL].td_maxend;
		c->tcp_rule.return_window_scale = ct->proto.tcp.seen[IP_CT_DIR_REPLY].td_scale;
		c->tcp_rule.return_max_window = ct->proto.tcp.seen[IP_CT_DIR_REPLY].td_maxwin;
		c->tcp_rule.return_end = ct->proto.tcp.seen[IP_CT_DIR_REPLY].td_end;
		c->tcp_rule.return_max_end = ct->proto.tcp.seen[IP_CT_DIR_REPLY].td_maxend;
		spin_unlock_bh(&ct->lock);
		c->valid_flags |= NSS_IPV4_RULE_CREATE_TCP_VALID;
		/* The kernel marked both directions liberal when it accepted
		 * the flow, so a window snapshot from before the handoff must
		 * not be enforced against packets it never saw.
		 */
		c->rule_flags |= NSS_IPV4_RULE_CREATE_FLAG_NO_SEQ_CHECK;
	}

	e->t4 = c->tuple;

	m->cm.interface = NSS_INTERFACE_IPV4;
	m->cm.type = NSS_IPV4_TX_CREATE_RULE_MSG;
	ret = nss_msg_send(core, m, offsetof(struct nss_ipv4_msg, msg) +
				    sizeof(*c));
	if (!ret && m->cm.error) {
		dev_warn_ratelimited(core->dev,
				     "v4 rule refused: error %u proto %u if %d->%d\n",
				     m->cm.error, e->protocol,
				     c->conn_rule.flow_interface_num,
				     c->conn_rule.return_interface_num);
		ret = -EIO;
	}

	return ret;
}

/* The rule carries each address word in host order, as the interface
 * requires; an address written any other way never matches. The public host
 * caller does the same per-word ntohl.
 */
static void nss_flow_addr6(u32 out[4], const union nf_inet_addr *in)
{
	int i;

	for (i = 0; i < 4; i++)
		out[i] = ntohl(in->ip6[i]);
}

static int nss_flow_send_v6(struct nss_core *core, struct nss_flow_entry *e,
			    struct nf_conn *ct)
{
	const struct nf_conntrack_tuple *o;
	struct nss_ipv6_rule_create_msg *c;
	struct nss_ipv6_msg *m __free(kfree) =
		kzalloc(sizeof(*m), GFP_KERNEL);
	int ret;

	if (!m)
		return -ENOMEM;

	o = &ct->tuplehash[IP_CT_DIR_ORIGINAL].tuple;
	c = &m->msg.rule_create;

	c->tuple.protocol = e->protocol;
	nss_flow_addr6(c->tuple.flow_ip, &o->src.u3);
	c->tuple.flow_ident = ntohs(o->src.u.all);
	nss_flow_addr6(c->tuple.return_ip, &o->dst.u3);
	c->tuple.return_ident = ntohs(o->dst.u.all);

	nss_flow_mac(c->conn_rule.flow_mac,
		     e->dir[FLOW_OFFLOAD_DIR_REPLY].mac);
	nss_flow_mac(c->conn_rule.return_mac,
		     e->dir[FLOW_OFFLOAD_DIR_ORIGINAL].mac);

	c->conn_rule.flow_interface_num =
		e->dir[FLOW_OFFLOAD_DIR_ORIGINAL].ifnum;
	c->conn_rule.return_interface_num =
		e->dir[FLOW_OFFLOAD_DIR_REPLY].ifnum;
	c->conn_rule.flow_mtu = e->dir[FLOW_OFFLOAD_DIR_ORIGINAL].mtu;
	c->conn_rule.return_mtu = e->dir[FLOW_OFFLOAD_DIR_REPLY].mtu;

	c->vlan_primary_rule.ingress_vlan_tag = NSS_FLOW_VLAN_NONE;
	c->vlan_primary_rule.egress_vlan_tag = NSS_FLOW_VLAN_NONE;
	c->vlan_secondary_rule.ingress_vlan_tag = NSS_FLOW_VLAN_NONE;
	c->vlan_secondary_rule.egress_vlan_tag = NSS_FLOW_VLAN_NONE;

	c->qos_rule.flow_qos_tag = e->priority;
	c->qos_rule.return_qos_tag = e->priority;

	c->nexthop_rule.flow_nexthop = c->conn_rule.flow_interface_num;
	c->nexthop_rule.return_nexthop = c->conn_rule.return_interface_num;

	c->rule_flags = NSS_IPV6_RULE_CREATE_FLAG_ROUTED;
	c->valid_flags = NSS_IPV6_RULE_CREATE_CONN_VALID |
			 NSS_IPV6_RULE_CREATE_QOS_VALID |
			 NSS_IPV6_RULE_CREATE_NEXTHOP_VALID;

	if (e->protocol == IPPROTO_TCP) {
		spin_lock_bh(&ct->lock);
		c->tcp_rule.flow_window_scale = ct->proto.tcp.seen[IP_CT_DIR_ORIGINAL].td_scale;
		c->tcp_rule.flow_max_window = ct->proto.tcp.seen[IP_CT_DIR_ORIGINAL].td_maxwin;
		c->tcp_rule.flow_end = ct->proto.tcp.seen[IP_CT_DIR_ORIGINAL].td_end;
		c->tcp_rule.flow_max_end = ct->proto.tcp.seen[IP_CT_DIR_ORIGINAL].td_maxend;
		c->tcp_rule.return_window_scale = ct->proto.tcp.seen[IP_CT_DIR_REPLY].td_scale;
		c->tcp_rule.return_max_window = ct->proto.tcp.seen[IP_CT_DIR_REPLY].td_maxwin;
		c->tcp_rule.return_end = ct->proto.tcp.seen[IP_CT_DIR_REPLY].td_end;
		c->tcp_rule.return_max_end = ct->proto.tcp.seen[IP_CT_DIR_REPLY].td_maxend;
		spin_unlock_bh(&ct->lock);
		c->valid_flags |= NSS_IPV6_RULE_CREATE_TCP_VALID;
		c->rule_flags |= NSS_IPV6_RULE_CREATE_FLAG_NO_SEQ_CHECK;
	}

	e->t6 = c->tuple;

	m->cm.interface = NSS_INTERFACE_IPV6;
	m->cm.type = NSS_IPV6_TX_CREATE_RULE_MSG;
	ret = nss_msg_send(core, m, offsetof(struct nss_ipv6_msg, msg) +
				    sizeof(*c));
	if (!ret && m->cm.error) {
		dev_warn_ratelimited(core->dev,
				     "v6 rule refused: error %u proto %u if %d->%d\n",
				     m->cm.error, e->protocol,
				     c->conn_rule.flow_interface_num,
				     c->conn_rule.return_interface_num);
		ret = -EIO;
	}

	return ret;
}

static void nss_flow_withdraw(struct nss_core *core, struct nss_flow_entry *e)
{
	if (!e->installed)
		return;

	if (e->v6) {
		struct nss_ipv6_msg *m __free(kfree) =
			kzalloc(sizeof(*m), GFP_KERNEL);

		if (!m)
			return;
		m->msg.rule_destroy.tuple = e->t6;
		m->cm.interface = NSS_INTERFACE_IPV6;
		m->cm.type = NSS_IPV6_TX_DESTROY_RULE_MSG;
		nss_msg_send(core, m, offsetof(struct nss_ipv6_msg, msg) +
				      sizeof(m->msg.rule_destroy));
	} else {
		struct nss_ipv4_msg *m __free(kfree) =
			kzalloc(sizeof(*m), GFP_KERNEL);

		if (!m)
			return;
		m->msg.rule_destroy.tuple = e->t4;
		m->cm.interface = NSS_INTERFACE_IPV4;
		m->cm.type = NSS_IPV4_TX_DESTROY_RULE_MSG;
		nss_msg_send(core, m, offsetof(struct nss_ipv4_msg, msg) +
				      sizeof(m->msg.rule_destroy));
	}

	e->installed = false;
}

static void nss_flow_forget(struct nss_core *core, struct nss_flow_entry *e)
{
	rhashtable_remove_fast(&nss_flow_table, &e->node, nss_flow_ht_params);
	nss_flow_withdraw(core, e);
	kfree_rcu(e, rcu);
}

static int nss_flow_replace(struct nss_core *core, struct flow_cls_offload *f)
{
	enum flow_offload_tuple_dir dir = nss_flow_dir_of(f->cookie);
	struct flow_offload *flow = nss_flow_of(f->cookie);
	unsigned long key = (unsigned long)flow;
	struct nss_flow_entry *e;
	struct flow_match_meta meta;
	struct flow_match_basic basic;
	struct flow_rule *rule = flow_cls_offload_flow_rule(f);
	int ret;

	if (!flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_META) ||
	    !flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_BASIC))
		return nss_flow_reject_at(NSS_FLOW_REJECT_KEY, -EOPNOTSUPP);

	e = rhashtable_lookup_fast(&nss_flow_table, &key, nss_flow_ht_params);
	if (!e) {
		/* Only a connection a station is one end of belongs here. The
		 * switch carries everything else, and a flow claimed by both
		 * would be forwarded twice. Either direction may be the
		 * wireless one, and the offer in hand names only its own
		 * ingress, so the other direction's is read off the flow.
		 */
		flow_rule_match_meta(rule, &meta);
		if (!nss_flow_is_wireless(meta.key->ingress_ifindex) &&
		    !nss_flow_is_wireless(flow->tuplehash[!dir].tuple.iifidx)) {
			nss_flow_last_ifindex = meta.key->ingress_ifindex;
			dev_warn_ratelimited(core->dev,
					     "flow not wireless: ingress %d, other %d\n",
					     meta.key->ingress_ifindex,
					     flow->tuplehash[!dir].tuple.iifidx);
			return nss_flow_reject_at(NSS_FLOW_REJECT_NOT_WIRELESS,
						  -EOPNOTSUPP);
		}

		e = kzalloc(sizeof(*e), GFP_KERNEL);
		if (!e)
			return -ENOMEM;

		e->flow = key;
		flow_rule_match_basic(rule, &basic);
		e->protocol = basic.key->ip_proto;
		e->v6 = basic.key->n_proto == htons(ETH_P_IPV6);
		e->lastused = jiffies;

		ret = rhashtable_insert_fast(&nss_flow_table, &e->node,
					     nss_flow_ht_params);
		if (ret) {
			kfree(e);
			return ret;
		}
	}

	ret = nss_flow_take_dir(core, e, dir, f);
	if (ret)
		goto drop;

	if (!e->dir[FLOW_OFFLOAD_DIR_ORIGINAL].seen ||
	    !e->dir[FLOW_OFFLOAD_DIR_REPLY].seen)
		return 0;

	/* The connection is only complete now, and a refresh of one already
	 * installed asks for nothing new.
	 */
	if (e->installed)
		return 0;

	if (!flow->ct) {
		ret = nss_flow_reject_at(NSS_FLOW_REJECT_NO_CT, -EOPNOTSUPP);
		goto drop;
	}

	ret = e->v6 ? nss_flow_send_v6(core, e, flow->ct) :
		      nss_flow_send_v4(core, e, flow->ct);
	if (ret) {
		nss_flow_reject_at(NSS_FLOW_REJECT_FIRMWARE, ret);
		goto drop;
	}

	e->installed = true;
	e->lastused = jiffies;
	nss_flow_pushed++;

	return 0;

drop:
	nss_flow_forget(core, e);

	return ret;
}

static int nss_flow_destroy(struct nss_core *core, struct flow_cls_offload *f)
{
	unsigned long key = (unsigned long)nss_flow_of(f->cookie);
	struct nss_flow_entry *e;

	e = rhashtable_lookup_fast(&nss_flow_table, &key, nss_flow_ht_params);
	if (!e)
		return -ENOENT;

	nss_flow_forget(core, e);

	return 0;
}

/* A record from the firmware's sweep marks its connection recently used if
 * any of its four counters moved since the previous sweep.
 */
static void nss_flow_sync_mark_v4(const struct nss_ipv4_conn_sync *r)
{
	struct rhashtable_iter iter;
	struct nss_flow_entry *e;

	if (!(r->flow_rx_packet_count | r->flow_tx_packet_count |
	      r->return_rx_packet_count | r->return_tx_packet_count))
		return;

	rhashtable_walk_enter(&nss_flow_table, &iter);
	rhashtable_walk_start(&iter);
	while ((e = rhashtable_walk_next(&iter)) != NULL) {
		if (IS_ERR(e))
			continue;
		if (e->v6 || !e->installed ||
		    e->t4.protocol != r->protocol ||
		    e->t4.flow_ip != r->flow_ip ||
		    e->t4.flow_ident != r->flow_ident ||
		    e->t4.return_ip != r->return_ip ||
		    e->t4.return_ident != r->return_ident)
			continue;
		e->lastused = jiffies;
		break;
	}
	rhashtable_walk_stop(&iter);
	rhashtable_walk_exit(&iter);
}

static void nss_flow_sync_mark_v6(const struct nss_ipv6_conn_sync *r)
{
	struct rhashtable_iter iter;
	struct nss_flow_entry *e;

	if (!(r->flow_rx_packet_count | r->flow_tx_packet_count |
	      r->return_rx_packet_count | r->return_tx_packet_count))
		return;

	rhashtable_walk_enter(&nss_flow_table, &iter);
	rhashtable_walk_start(&iter);
	while ((e = rhashtable_walk_next(&iter)) != NULL) {
		if (IS_ERR(e))
			continue;
		if (!e->v6 || !e->installed ||
		    e->t6.protocol != r->protocol ||
		    memcmp(e->t6.flow_ip, r->flow_ip, sizeof(r->flow_ip)) ||
		    e->t6.flow_ident != r->flow_ident ||
		    memcmp(e->t6.return_ip, r->return_ip, sizeof(r->return_ip)) ||
		    e->t6.return_ident != r->return_ident)
			continue;
		e->lastused = jiffies;
		break;
	}
	rhashtable_walk_stop(&iter);
	rhashtable_walk_exit(&iter);
}

/* Sweep the firmware's connection table for what moved.
 *
 * The request names a start index and how much room the answer has; the
 * answer carries as many per-connection records as fit and the index to ask
 * from next, zero once the table has wrapped. After the first sweep the
 * per-connection sync messages stop arriving on their own, which otherwise
 * cost one message every couple of milliseconds per connection.
 */
static void nss_flow_sweep_v4(struct nss_core *core)
{
	struct nss_ipv4_msg *m __free(kfree) =
		kzalloc(NSS_EMPTY_BUFFER_SIZE, GFP_KERNEL);
	struct nss_ipv4_conn_sync_many_msg *s;
	int rounds = 8;
	u16 index = 0;
	int i;

	if (!m)
		return;
	s = &m->msg.conn_stats_many;

	do {
		memset(m, 0, sizeof(m->cm) + sizeof(*s));
		m->cm.interface = NSS_INTERFACE_IPV4;
		m->cm.type = NSS_IPV4_TX_CONN_STATS_SYNC_MANY_MSG;
		s->index = index;
		s->size = NSS_EMPTY_BUFFER_SIZE;
		if (nss_msg_transact(core, m,
				     offsetof(struct nss_ipv4_msg, msg) + sizeof(*s),
				     NSS_EMPTY_BUFFER_SIZE))
			return;
		/* The count is the firmware's; the room is not. */
		for (i = 0; i < s->count &&
		     offsetof(struct nss_ipv4_msg, msg) + sizeof(*s) +
		     (i + 1) * sizeof(s->conn_sync[0]) <= NSS_EMPTY_BUFFER_SIZE;
		     i++)
			nss_flow_sync_mark_v4(&s->conn_sync[i]);
		index = s->next;
	} while (index && --rounds);
}

static void nss_flow_sweep_v6(struct nss_core *core)
{
	struct nss_ipv6_msg *m __free(kfree) =
		kzalloc(NSS_EMPTY_BUFFER_SIZE, GFP_KERNEL);
	struct nss_ipv6_conn_sync_many_msg *s;
	int rounds = 8;
	u16 index = 0;
	int i;

	if (!m)
		return;
	s = &m->msg.conn_stats_many;

	do {
		memset(m, 0, sizeof(m->cm) + sizeof(*s));
		m->cm.interface = NSS_INTERFACE_IPV6;
		m->cm.type = NSS_IPV6_TX_CONN_STATS_SYNC_MANY_MSG;
		s->index = index;
		s->size = NSS_EMPTY_BUFFER_SIZE;
		if (nss_msg_transact(core, m,
				     offsetof(struct nss_ipv6_msg, msg) + sizeof(*s),
				     NSS_EMPTY_BUFFER_SIZE))
			return;
		for (i = 0; i < s->count &&
		     offsetof(struct nss_ipv6_msg, msg) + sizeof(*s) +
		     (i + 1) * sizeof(s->conn_sync[0]) <= NSS_EMPTY_BUFFER_SIZE;
		     i++)
			nss_flow_sync_mark_v6(&s->conn_sync[i]);
		index = s->next;
	} while (index && --rounds);
}

/* What the kernel asks in place of seeing the packets.
 *
 * A connection the firmware carries stops reaching conntrack, so the kernel
 * would age it out on its own schedule unless something says it is still
 * alive. The kernel asks per flow but the firmware answers per table, so one
 * sweep a second serves every flow's answer.
 */
static unsigned long nss_flow_swept;

static int nss_flow_stats(struct nss_core *core, struct flow_cls_offload *f)
{
	unsigned long key = (unsigned long)nss_flow_of(f->cookie);
	struct nss_flow_entry *e;

	e = rhashtable_lookup_fast(&nss_flow_table, &key, nss_flow_ht_params);
	if (!e || !e->installed)
		return -ENOENT;

	if (time_after(jiffies, nss_flow_swept + HZ)) {
		nss_flow_swept = jiffies;
		nss_flow_sweep_v4(core);
		nss_flow_sweep_v6(core);
	}

	flow_stats_update(&f->stats, 0, 0, 0, e->lastused,
			  FLOW_ACTION_HW_STATS_DELAYED);

	return 0;
}

static int nss_flow_block_cb(enum tc_setup_type type, void *type_data,
			     void *cb_priv)
{
	struct flow_cls_offload *f = type_data;
	struct nss_core *core;

	if (type != TC_SETUP_CLSFLOWER)
		return -EOPNOTSUPP;

	guard(mutex)(&nss_flow_lock);

	core = nss_flow_core;
	if (!core || !core->running)
		return -EOPNOTSUPP;

	switch (f->command) {
	case FLOW_CLS_REPLACE:
		return nss_flow_replace(core, f);
	case FLOW_CLS_DESTROY:
		return nss_flow_destroy(core, f);
	case FLOW_CLS_STATS:
		return nss_flow_stats(core, f);
	default:
		return -EOPNOTSUPP;
	}
}

static void nss_flow_block_release(void *cb_priv)
{
}

/* Take the flow table's block on a wireless device.
 *
 * The kernel offers the block through the device's own traffic-control entry
 * point, which for a wireless device the subsystem forwards to its driver. So
 * the driver of the radio asks for this, and nothing in the subsystem has to
 * change to let it.
 */
int nss_flow_setup_block(struct net_device *dev, struct flow_block_offload *f)
{
	struct flow_block_cb *block_cb;

	if (f->binder_type != FLOW_BLOCK_BINDER_TYPE_CLSACT_INGRESS)
		return -EOPNOTSUPP;

	f->driver_block_list = &nss_flow_blocks;

	switch (f->command) {
	case FLOW_BLOCK_BIND:
		block_cb = flow_block_cb_lookup(f->block, nss_flow_block_cb,
						dev);
		if (block_cb) {
			flow_block_cb_incref(block_cb);
			return 0;
		}

		block_cb = flow_block_cb_alloc(nss_flow_block_cb, dev, dev,
					       nss_flow_block_release);
		if (IS_ERR(block_cb))
			return PTR_ERR(block_cb);

		flow_block_cb_incref(block_cb);
		flow_block_cb_add(block_cb, f);
		list_add_tail(&block_cb->driver_list, &nss_flow_blocks);
		nss_flow_bound++;
		return 0;
	case FLOW_BLOCK_UNBIND:
		block_cb = flow_block_cb_lookup(f->block, nss_flow_block_cb,
						dev);
		if (!block_cb)
			return -ENOENT;

		if (!flow_block_cb_decref(block_cb)) {
			flow_block_cb_remove(block_cb, f);
			list_del(&block_cb->driver_list);
		}
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}
EXPORT_SYMBOL_GPL(nss_flow_setup_block);

void nss_flow_print(struct seq_file *s)
{
	struct rhashtable_iter iter;
	struct nss_flow_entry *e;
	int i;

	seq_printf(s, "bound %u pushed %u last-ifindex %d\n",
		   nss_flow_bound, nss_flow_pushed, nss_flow_last_ifindex);
	seq_puts(s, "rejected");
	for (i = 0; i < NSS_FLOW_REJECT_MAX; i++)
		seq_printf(s, " %s %u", nss_flow_reject_name[i],
			   nss_flow_reject[i]);
	seq_putc(s, '\n');

	rhashtable_walk_enter(&nss_flow_table, &iter);
	rhashtable_walk_start(&iter);
	while ((e = rhashtable_walk_next(&iter)) != NULL) {
		if (IS_ERR(e))
			continue;
		seq_printf(s, "proto %u %s if %d/%d prio %u installed %d age %ums\n",
			   e->protocol, e->v6 ? "v6" : "v4",
			   e->dir[FLOW_OFFLOAD_DIR_ORIGINAL].ifnum,
			   e->dir[FLOW_OFFLOAD_DIR_REPLY].ifnum,
			   e->priority, e->installed,
			   jiffies_to_msecs(jiffies - e->lastused));
	}
	rhashtable_walk_stop(&iter);
	rhashtable_walk_exit(&iter);
}

/* Halting a core takes every rule it held with it, so the entries only
 * mislead after that; the withdrawals they would send have nowhere to go.
 */
void nss_flow_flush(void)
{
	struct rhashtable_iter iter;
	struct nss_flow_entry *e;

	guard(mutex)(&nss_flow_lock);

	rhashtable_walk_enter(&nss_flow_table, &iter);
	rhashtable_walk_start(&iter);
	while ((e = rhashtable_walk_next(&iter)) != NULL) {
		if (IS_ERR(e))
			continue;
		rhashtable_remove_fast(&nss_flow_table, &e->node,
				       nss_flow_ht_params);
		kfree_rcu(e, rcu);
	}
	rhashtable_walk_stop(&iter);
	rhashtable_walk_exit(&iter);
}

int nss_flow_init(void)
{
	int ret;

	ret = rhashtable_init(&nss_flow_table, &nss_flow_ht_params);
	if (ret)
		return ret;

	nss_flow_ready = true;

	return 0;
}

static void nss_flow_free(void *ptr, void *arg)
{
	kfree(ptr);
}

void nss_flow_exit(void)
{
	if (!nss_flow_ready)
		return;

	rhashtable_free_and_destroy(&nss_flow_table, nss_flow_free, NULL);
	nss_flow_ready = false;
}
