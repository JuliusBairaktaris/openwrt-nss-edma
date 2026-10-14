// SPDX-License-Identifier: GPL-2.0-only
/* Carrying a cellular modem's connections in the firmware.
 *
 * A modem's network device carries bare IP packets, with no link-layer header
 * and no switch port behind it, so neither the switch nor any interface the
 * firmware already has can reach it. The firmware reaches such a device through
 * a pair of host-side nodes: what the host hands to one is routed as if it had
 * arrived on the device, and what the firmware routes to the other comes back
 * to the host to be sent there. With the device named by that second node, a
 * rule between it and a station or a switch port is a rule like any other, and
 * the flow back end needs only to know the number.
 *
 * The node type the vendor used for modems, rmnet_rx, lives only in the image
 * core 1 runs: core 0 refuses to allocate one. The generic redirect pair is
 * what core 0 has, and it is the same shape - a node pair with a sibling and a
 * next hop each, configured with the virtual interface message.
 *
 * The node the host hands a packet to feeds the firmware's ethernet node, so
 * a packet goes in framed; a packet the firmware does not route comes back,
 * still framed, and goes up the stack as if it had just arrived; one it routes
 * to the device comes back framed with the other side's addresses. Nothing a
 * modem sends or receives ever carries those frames.
 *
 * The device exists long before the core is up, and a core that faults and
 * restarts loses every node it had, so the pair is made wherever a running
 * core meets a device that is up: when the device comes up, and when the core
 * arms its ports. A halt only forgets the pair; the next arm makes a new one.
 */

#include <linux/cleanup.h>
#include <linux/etherdevice.h>
#include <linux/if_arp.h>
#include <linux/ipv6.h>
#include <linux/netdevice.h>
#include <linux/netfilter.h>
#include <linux/netfilter_netdev.h>
#include <linux/rtnetlink.h>
#include <linux/seq_file.h>
#include <linux/skbuff.h>
#include <linux/tcp.h>
#include <net/ip.h>
#include <net/ipv6.h>
#include <net/netfilter/nf_conntrack.h>

#include "nss_drv.h"

/* The virtual interface's own configuration message, sent to each node of
 * the pair (public nss_virt_if.h).
 */
#define NSS_VIRT_IF_TX_CONFIG_MSG	10000

struct nss_virt_if_config_msg {
	u32 flags;
	u32 sibling;
	u32 nexthop;
	u8 mac_addr[ETH_ALEN];
};

/* A modem has one device per multiplexed channel, and a router uses one or
 * two of them.
 */
#define NSS_RAWIP_MAX	4

/* The source address of the frame a modem's packet is put in. The firmware
 * takes the source interface from the node, not from the frame, but an
 * all-zero or own address is the kind of thing an ethernet node may drop. The
 * modem driver uses this one for its own ethernet mode.
 */
static const u8 nss_rawip_peer[ETH_ALEN] = { 0x02, 0x50, 0xf3, 0x00, 0x00, 0x00 };

struct nss_rawip {
	struct net_device *dev;	/* NULL: slot free */
	int ifindex;
	int n2h;		/* -1 until a running core gives the pair */
	int h2n;
	struct nf_hook_ops ops;	/* ahead of the flow table on the device */
	bool hooked;
	u64 inject;
	u64 inject_fail;
	u64 back;		/* came back unrouted, delivered up */
	u64 xmit;		/* routed to the modem by the firmware */
	u64 drop;
};

/* Written under the RTNL, read from the receive paths. */
static struct nss_rawip nss_rawip_tbl[NSS_RAWIP_MAX];
static struct nss_core *nss_rawip_core;
static bool nss_rawip_ready;

/* Set while a packet the firmware gave back goes up the stack, so the receive
 * handler lets it through instead of handing it down again. The delivery stays
 * on this CPU, which is what makes a per-CPU flag enough.
 */
static DEFINE_PER_CPU(bool, nss_rawip_reinject);

static bool nss_rawip_enable = true;
module_param_named(rawip, nss_rawip_enable, bool, 0644);
MODULE_PARM_DESC(rawip, "carry raw-IP (cellular) devices in the firmware; read when a device comes up");

static bool nss_rawip_wanted(const struct net_device *dev)
{
	return dev->type == ARPHRD_RAWIP && dev->addr_len == ETH_ALEN;
}

static struct nss_rawip *nss_rawip_find(const struct net_device *dev)
{
	int i;

	for (i = 0; i < NSS_RAWIP_MAX; i++)
		if (nss_rawip_tbl[i].dev == dev)
			return &nss_rawip_tbl[i];

	return NULL;
}

/* Whether @ifindex is a device the firmware can route to. For the flow back
 * end, which decides from an ifindex before it looks for a number.
 */
bool nss_rawip_bound(int ifindex)
{
	int i;

	for (i = 0; i < NSS_RAWIP_MAX; i++)
		if (READ_ONCE(nss_rawip_tbl[i].ifindex) == ifindex &&
		    READ_ONCE(nss_rawip_tbl[i].n2h) >= 0)
			return true;

	return false;
}

static int nss_rawip_config(struct nss_core *core, int if_num, int sibling,
			    int nexthop, const u8 *mac)
{
	struct nss_virt_if_config_msg c = {};

	c.sibling = sibling;
	c.nexthop = nexthop;
	ether_addr_copy(c.mac_addr, mac);

	return nss_phys_msg(core, if_num, NSS_VIRT_IF_TX_CONFIG_MSG, &c,
			    sizeof(c));
}

static int nss_rawip_dyn_alloc(struct nss_core *core, u32 type)
{
	struct nss_dynamic_interface_msg d = {};
	int ret;

	d.cm.interface = NSS_INTERFACE_DYNAMIC;
	d.cm.type = NSS_DYNAMIC_INTERFACE_ALLOC_NODE;
	d.msg.alloc_node.type = type;
	ret = nss_msg_send(core, &d, sizeof(d));
	if (ret)
		return ret;
	if (d.msg.alloc_node.if_num < 0 ||
	    d.msg.alloc_node.if_num >= NSS_INTERFACE_MAX)
		return -ERANGE;

	return d.msg.alloc_node.if_num;
}

static void nss_rawip_dyn_free(struct nss_core *core, u32 type, int if_num)
{
	struct nss_dynamic_interface_msg d = {};

	d.cm.interface = NSS_INTERFACE_DYNAMIC;
	d.cm.type = NSS_DYNAMIC_INTERFACE_DEALLOC_NODE;
	d.msg.dealloc_node.type = type;
	d.msg.dealloc_node.if_num = if_num;
	nss_msg_send(core, &d, sizeof(d));
}

/* Give @r a node pair on @core. The host-to-firmware node feeds the ethernet
 * node, which is where the connection engines are; the other hands what the
 * firmware routes there to the host. A single core needs no core-to-core hop.
 */
static int nss_rawip_pair(struct nss_core *core, struct nss_rawip *r)
{
	int n2h, h2n, ret;

	n2h = nss_rawip_dyn_alloc(core, NSS_DYNAMIC_INTERFACE_TYPE_GENERIC_REDIR_N2H);
	if (n2h < 0)
		return n2h;

	h2n = nss_rawip_dyn_alloc(core, NSS_DYNAMIC_INTERFACE_TYPE_GENERIC_REDIR_H2N);
	if (h2n < 0) {
		ret = h2n;
		goto n2h;
	}

	ret = nss_rawip_config(core, n2h, h2n, NSS_INTERFACE_N2H,
			       r->dev->dev_addr);
	if (ret)
		goto h2n;
	ret = nss_rawip_config(core, h2n, n2h, NSS_INTERFACE_ETH_RX,
			       r->dev->dev_addr);
	if (ret)
		goto h2n;

	/* The flow back end finds a device by its slot in the interface
	 * table, and the slot is the node the firmware sends towards it.
	 */
	dev_hold(r->dev);
	rcu_assign_pointer(core->iface[n2h], r->dev);
	WRITE_ONCE(r->h2n, h2n);
	WRITE_ONCE(r->n2h, n2h);

	netdev_info(r->dev, "routed by NSS core %u, nodes %d/%d\n", core->id,
		    n2h, h2n);

	return 0;

h2n:
	nss_rawip_dyn_free(core, NSS_DYNAMIC_INTERFACE_TYPE_GENERIC_REDIR_H2N, h2n);
n2h:
	nss_rawip_dyn_free(core, NSS_DYNAMIC_INTERFACE_TYPE_GENERIC_REDIR_N2H, n2h);
	netdev_warn(r->dev, "no NSS node pair: %d\n", ret);

	return ret;
}

/* Give the pair back while the core still runs. */
static void nss_rawip_unpair(struct nss_core *core, struct nss_rawip *r)
{
	int n2h = r->n2h, h2n = r->h2n;
	struct net_device *dev;

	if (n2h < 0)
		return;

	/* Out of every lookup first, so that no rule towards the node can be
	 * made while the ones already made are withdrawn.
	 */
	WRITE_ONCE(r->n2h, -1);
	WRITE_ONCE(r->h2n, -1);

	dev = rcu_replace_pointer(core->iface[n2h], NULL,
				  lockdep_is_held(&core->lock));
	synchronize_net();
	if (dev)
		dev_put(dev);

	/* Rules that send towards the node outlive it in the firmware until
	 * the flow table tears them down, which starts only once the device
	 * is down; a node allocated meanwhile could get the same number.
	 */
	if (core->running)
		nss_flow_withdraw_ifnum(core, n2h);

	if (core->running) {
		nss_rawip_dyn_free(core, NSS_DYNAMIC_INTERFACE_TYPE_GENERIC_REDIR_H2N,
				   h2n);
		nss_rawip_dyn_free(core, NSS_DYNAMIC_INTERFACE_TYPE_GENERIC_REDIR_N2H,
				   n2h);
	}
}

/* Whether a packet from the modem is one the firmware could route: unicast,
 * unfragmented TCP or UDP. Neighbour discovery, router advertisements,
 * fragments and the rest go straight up, as they would without the firmware.
 * So do TCP segments that open or close a connection: no rule can be waiting
 * for one that opens, and one that closes is for the host's connection
 * tracking to see, which ends the rule.
 */
static bool nss_rawip_routable(struct sk_buff *skb, __be16 proto)
{
	unsigned int thoff;
	u8 l4;

	if (proto == htons(ETH_P_IP)) {
		const struct iphdr *iph;

		if (!pskb_may_pull(skb, sizeof(*iph)))
			return false;
		iph = ip_hdr(skb);
		if (iph->ihl != 5 || ip_is_fragment(iph) ||
		    ipv4_is_multicast(iph->daddr) || ipv4_is_lbcast(iph->daddr))
			return false;
		l4 = iph->protocol;
		thoff = sizeof(*iph);
	} else {
		const struct ipv6hdr *ip6h;

		if (!pskb_may_pull(skb, sizeof(*ip6h)))
			return false;
		ip6h = ipv6_hdr(skb);
		if (ipv6_addr_is_multicast(&ip6h->daddr))
			return false;
		l4 = ip6h->nexthdr;
		thoff = sizeof(*ip6h);
	}

	if (l4 == IPPROTO_UDP)
		return true;
	if (l4 != IPPROTO_TCP || !pskb_may_pull(skb, thoff + sizeof(struct tcphdr)))
		return false;

	skb_set_transport_header(skb, thoff);

	return !(tcp_hdr(skb)->syn || tcp_hdr(skb)->fin || tcp_hdr(skb)->rst);
}

static __be16 nss_rawip_proto(const struct sk_buff *skb, unsigned int off)
{
	if (skb->len <= off)
		return 0;

	switch (skb->data[off] & 0xf0) {
	case 0x40:
		return htons(ETH_P_IP);
	case 0x60:
		return htons(ETH_P_IPV6);
	default:
		return 0;
	}
}

/* Hand a packet the modem delivered to the firmware, framed. False leaves it
 * with the host as it was; @skb must not be shared.
 */
static bool nss_rawip_inject(struct nss_rawip *r, struct sk_buff *skb)
{
	struct nss_core *core = READ_ONCE(nss_rawip_core);
	int h2n = READ_ONCE(r->h2n);
	struct ethhdr *eth;
	__be16 proto;

	if (!core || h2n < 0 || !READ_ONCE(core->running))
		return false;

	if (!pskb_may_pull(skb, 1))
		return false;
	proto = nss_rawip_proto(skb, 0);
	if (!proto || skb->len > skb->dev->mtu)
		return false;

	skb_reset_network_header(skb);
	if (!nss_rawip_routable(skb, proto))
		return false;

	/* Four more than the frame, for the word alignment the send may need. */
	if (skb_cow_head(skb, ETH_HLEN + 4)) {
		r->inject_fail++;
		return false;
	}

	eth = skb_push(skb, ETH_HLEN);
	ether_addr_copy(eth->h_dest, skb->dev->dev_addr);
	ether_addr_copy(eth->h_source, nss_rawip_peer);
	eth->h_proto = proto;

	if (nss_data_send_flags(core, skb, h2n, NSS_H2N_FLAG_VIRTUAL_BUFFER)) {
		/* Still the host's. The send may have moved the data down
		 * to a word boundary, which the headers do not follow.
		 */
		skb_pull(skb, ETH_HLEN);
		skb_reset_network_header(skb);
		skb_reset_mac_header(skb);
		r->inject_fail++;
		return false;
	}

	r->inject++;

	return true;
}

/* The device's first ingress hook, ahead of the flow table's: with the device
 * in the flow table, the table would otherwise forward an offloaded
 * connection's packets itself before the receive handler saw them.
 */
static unsigned int nss_rawip_hook(void *priv, struct sk_buff *skb,
				   const struct nf_hook_state *state)
{
	if (__this_cpu_read(nss_rawip_reinject) || skb_shared(skb))
		return NF_ACCEPT;

	return nss_rawip_inject(priv, skb) ? NF_STOLEN : NF_ACCEPT;
}

/* What skips the ingress hooks: a packet an ingress shaper redirected and
 * gives back, and anything while no hook is in place.
 */
static rx_handler_result_t nss_rawip_rx(struct sk_buff **pskb)
{
	struct sk_buff *skb = *pskb;

	if (__this_cpu_read(nss_rawip_reinject) ||
	    skb->pkt_type == PACKET_LOOPBACK)
		return RX_HANDLER_PASS;

	skb = skb_share_check(skb, GFP_ATOMIC);
	if (!skb)
		return RX_HANDLER_CONSUMED;
	*pskb = skb;

	if (!nss_rawip_inject(rcu_dereference(skb->dev->rx_handler_data), skb))
		return RX_HANDLER_PASS;

	return RX_HANDLER_CONSUMED;
}

static struct nss_rawip *nss_rawip_of_node(u32 if_num)
{
	int i;

	for (i = 0; i < NSS_RAWIP_MAX; i++)
		if (READ_ONCE(nss_rawip_tbl[i].n2h) == (int)if_num ||
		    READ_ONCE(nss_rawip_tbl[i].h2n) == (int)if_num)
			return &nss_rawip_tbl[i];

	return NULL;
}

/* A buffer the firmware handed up on one of the nodes: a packet it did not
 * route, to go up the stack, or one it routed to the modem, to be sent there.
 * Either may be a buffer the host lent it or one the host sent; the
 * descriptor says where the packet is in either.
 */
bool nss_rawip_recv(struct nss_core *core, const struct n2h_descriptor *desc,
		    struct sk_buff *skb)
{
	bool tx = NSS_SKB_CB(skb)->tx;
	u32 end = (u32)desc->payload_offs + desc->payload_len;
	struct net_device *dev;
	struct nss_rawip *r;
	__be16 proto;
	u32 size;
	int n2h;

	if (desc->buffer_type != NSS_N2H_BUFFER_PACKET &&
	    desc->buffer_type != NSS_N2H_BUFFER_PACKET_VIRTUAL)
		return false;

	/* Only a buffer the host sent down for the firmware to route can come
	 * back with something in it worth reading.
	 */
	if (tx && !NSS_SKB_CB(skb)->bidir)
		return false;

	r = nss_rawip_of_node(desc->interface_num);
	if (!r)
		return false;

	guard(rcu)();

	if (tx) {
		size = skb_end_pointer(skb) - skb->head;
		dma_unmap_single(core->dev, NSS_SKB_CB(skb)->dma, size,
				 DMA_BIDIRECTIONAL);
	} else {
		size = NSS_EMPTY_BUFFER_ALLOC;
		dma_unmap_single(core->dev, desc->buffer, size, DMA_FROM_DEVICE);
	}

	n2h = READ_ONCE(r->n2h);
	if (n2h < 0)
		goto drop;
	dev = rcu_dereference(core->iface[n2h]);
	if (!dev || end > size || skb_is_nonlinear(skb))
		goto drop;

	/* A sent buffer's own pointers describe what was sent, not what came
	 * back, so both kinds are placed by the descriptor.
	 */
	skb->data = skb->head + desc->payload_offs;
	skb->len = desc->payload_len;
	skb_set_tail_pointer(skb, skb->len);

	/* Framed on the way in, and framed again by the firmware on the way
	 * out; a packet that somehow comes back bare is taken as it is.
	 */
	proto = nss_rawip_proto(skb, ETH_HLEN);
	if (proto && skb->len >= ETH_HLEN &&
	    ((struct ethhdr *)skb->data)->h_proto == proto)
		skb_pull(skb, ETH_HLEN);
	else
		proto = nss_rawip_proto(skb, 0);
	if (!proto)
		goto drop;

	memset(skb->cb, 0, sizeof(skb->cb));
	skb->dev = dev;
	skb->protocol = proto;
	skb->ip_summed = CHECKSUM_NONE;
	skb_reset_mac_header(skb);
	skb_reset_network_header(skb);
	skb->mac_len = 0;

	if (desc->buffer_type == NSS_N2H_BUFFER_PACKET_VIRTUAL) {
		r->xmit++;
		dev_queue_xmit(skb);
		return true;
	}

	/* What was attached to the packet before it went down is still
	 * attached, and netfilter takes a packet that arrives with state for
	 * one it has seen: the reply of a connection the router opened would
	 * never be taken as established.
	 */
	nf_reset_ct(skb);
	skb_dst_drop(skb);
	skb->pkt_type = PACKET_HOST;

	skb->skb_iif = dev->ifindex;

	r->back++;
	__this_cpu_write(nss_rawip_reinject, true);
	/* The flow table's hook runs again: it forwards what it knows, and
	 * a reply passing it is what lets it offload the reply direction too.
	 * The taps and an ingress shaper saw the packet already; a second pass
	 * would charge the shaper twice, and its redirect would bring the
	 * packet back to the receive handler and down again.
	 */
	if (nf_hook_ingress(skb) >= 0) {
		skb->tc_skip_classify = 1;
		netif_receive_skb_core(skb);
	}
	__this_cpu_write(nss_rawip_reinject, false);

	return true;

drop:
	r->drop++;
	dev_kfree_skb_any(skb);

	return true;
}

/* Take @dev into the table and, with a running core, give it a pair. Under
 * the RTNL, and under the core's lock when there is one.
 */
static void nss_rawip_add(struct nss_core *core, struct net_device *dev)
{
	struct nss_rawip *r;
	int i;

	if (!nss_rawip_enable || !nss_rawip_wanted(dev))
		return;

	r = nss_rawip_find(dev);
	if (!r) {
		for (i = 0; i < NSS_RAWIP_MAX && !r; i++)
			if (!nss_rawip_tbl[i].dev)
				r = &nss_rawip_tbl[i];
		if (!r) {
			netdev_warn(dev, "no room for another raw-IP device\n");
			return;
		}

		*r = (struct nss_rawip){ .dev = dev, .n2h = -1, .h2n = -1 };
		if (netdev_rx_handler_register(dev, nss_rawip_rx, r)) {
			netdev_warn(dev, "has a receive handler already\n");
			*r = (struct nss_rawip){ .n2h = -1, .h2n = -1 };
			return;
		}
		WRITE_ONCE(r->ifindex, dev->ifindex);

		r->ops = (struct nf_hook_ops){
			.hook = nss_rawip_hook,
			.pf = NFPROTO_NETDEV,
			.hooknum = NF_NETDEV_INGRESS,
			.priority = INT_MIN + 1,
			.dev = dev,
			.priv = r,
		};
		if (nf_register_net_hook(dev_net(dev), &r->ops))
			netdev_warn(dev, "no ingress hook: flow table devices bypass the firmware\n");
		else
			r->hooked = true;
	}

	if (core && core->running && r->n2h < 0)
		nss_rawip_pair(core, r);
}

static void nss_rawip_del(struct nss_core *core, struct net_device *dev)
{
	struct nss_rawip *r = nss_rawip_find(dev);

	if (!r)
		return;

	if (r->hooked)
		nf_unregister_net_hook(dev_net(dev), &r->ops);
	r->hooked = false;
	if (core)
		nss_rawip_unpair(core, r);
	netdev_rx_handler_unregister(dev);
	WRITE_ONCE(r->ifindex, 0);
	WRITE_ONCE(r->n2h, -1);
	WRITE_ONCE(r->h2n, -1);
	r->dev = NULL;
}

/* Every device that is up, on a core that has just armed its ports. Under
 * the RTNL and the core's lock, as the arming is.
 */
void nss_rawip_arm(struct nss_core *core)
{
	struct net_device *dev;

	ASSERT_RTNL();

	/* One core carries the modems: the first to arm. A core on its way
	 * out arms once more from its halt, and must not come back here.
	 */
	if (!READ_ONCE(core->up) ||
	    (nss_rawip_core && nss_rawip_core != core))
		return;

	WRITE_ONCE(nss_rawip_core, core);
	if (!core->running)
		return;

	for_each_netdev(&init_net, dev)
		if (dev->flags & IFF_UP)
			nss_rawip_add(core, dev);
}

/* The core is going into reset and its nodes with it. Its interface table is
 * emptied by the halt, so only the numbers are forgotten here; the receive
 * handler passes everything up until the next arm.
 */
void nss_rawip_halt(struct nss_core *core)
{
	int i;

	if (READ_ONCE(nss_rawip_core) != core)
		return;

	for (i = 0; i < NSS_RAWIP_MAX; i++) {
		WRITE_ONCE(nss_rawip_tbl[i].h2n, -1);
		WRITE_ONCE(nss_rawip_tbl[i].n2h, -1);
	}
}

/* The core is going away, halted and with its work done; neither the
 * notifier nor the receive handler may reach it after this.
 */
void nss_rawip_detach(struct nss_core *core)
{
	rtnl_lock();
	if (nss_rawip_core == core)
		WRITE_ONCE(nss_rawip_core, NULL);
	rtnl_unlock();
	synchronize_net();
}

static int nss_rawip_event(struct notifier_block *nb, unsigned long event,
			   void *ptr)
{
	struct net_device *dev = netdev_notifier_info_to_dev(ptr);
	struct nss_core *core = READ_ONCE(nss_rawip_core);

	switch (event) {
	case NETDEV_UP:
		if (!nss_rawip_wanted(dev))
			break;
		if (core) {
			guard(mutex)(&core->lock);
			nss_rawip_add(core, dev);
		} else {
			nss_rawip_add(NULL, dev);
		}
		break;
	case NETDEV_GOING_DOWN:
	case NETDEV_UNREGISTER:
		/* Gone from the table by its slot, not by what it is now. */
		if (!nss_rawip_find(dev))
			break;
		if (core) {
			guard(mutex)(&core->lock);
			nss_rawip_del(core, dev);
		} else {
			nss_rawip_del(NULL, dev);
		}
		break;
	}

	return NOTIFY_DONE;
}

static struct notifier_block nss_rawip_nb = {
	.notifier_call = nss_rawip_event,
};

void nss_rawip_print(struct seq_file *s)
{
	int i;

	seq_printf(s, "enabled %d\n", nss_rawip_enable);

	rtnl_lock();
	for (i = 0; i < NSS_RAWIP_MAX; i++) {
		const struct nss_rawip *r = &nss_rawip_tbl[i];

		if (!r->dev)
			continue;
		seq_printf(s, "%s ifindex %d nodes %d/%d inject %llu inject-fail %llu back %llu xmit %llu drop %llu\n",
			   netdev_name(r->dev), r->ifindex, r->n2h, r->h2n,
			   r->inject, r->inject_fail, r->back, r->xmit,
			   r->drop);
	}
	rtnl_unlock();
}

int nss_rawip_init(void)
{
	int i, ret;

	/* A free slot must not match node 0, which is where most of what the
	 * firmware hands up arrives.
	 */
	for (i = 0; i < NSS_RAWIP_MAX; i++)
		nss_rawip_tbl[i] = (struct nss_rawip){ .n2h = -1, .h2n = -1 };

	ret = register_netdevice_notifier(&nss_rawip_nb);
	if (!ret)
		nss_rawip_ready = true;

	return ret;
}

/* After the cores are gone, so there is nothing to give back but the
 * hooks and receive handlers.
 */
void nss_rawip_exit(void)
{
	if (!nss_rawip_ready)
		return;

	/* Replays the going-down and unregister events of every device,
	 * which gives back each slot and its handlers.
	 */
	unregister_netdevice_notifier(&nss_rawip_nb);
	WRITE_ONCE(nss_rawip_core, NULL);

	nss_rawip_ready = false;
}
