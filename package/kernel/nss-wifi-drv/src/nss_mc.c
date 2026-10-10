// SPDX-License-Identifier: GPL-2.0-only
/* Routed multicast offload from the kernel's multicast routing cache.
 *
 * The MFC gives source, group, ingress and egress interfaces; the firmware
 * rule also needs the UDP ports, so they are learnt from the first packets of
 * a known (S,G) that reach the host. A bridge egress is expanded into its
 * firmware-owned member ports: the MDB ports while snooping has a querier,
 * otherwise every forwarding port with multicast flooding on, which is what
 * br_dev_xmit() itself does. An entry with any egress the firmware does not
 * own stays in software.
 *
 * The MFC notifier runs in atomic context, so every event is queued and
 * handled in one work item under rtnl, which also stabilises the MFC entry
 * and the bridge state it reads.
 */

#include <linux/cleanup.h>
#include <linux/etherdevice.h>
#include <linux/if_arp.h>
#include <linux/if_bridge.h>
#include <linux/if_vlan.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/mroute.h>
#include <linux/mroute6.h>
#include <linux/mutex.h>
#include <linux/netdevice.h>
#include <linux/netfilter.h>
#include <linux/netfilter_ipv4.h>
#include <linux/netfilter_ipv6.h>
#include <linux/rtnetlink.h>
#include <linux/udp.h>
#include <net/fib_notifier.h>
#include <net/if_inet6.h>
#include <net/ip.h>
#include <net/ip6_fib.h>
#include <net/switchdev.h>

#include "nss_drv.h"

#define NSS_MC_VLAN_NONE	0xfff
/* ponytail: port pairs per (S,G); one stream rarely uses more than one */
#define NSS_MC_PORTS		4

enum nss_mc_why {
	NSS_MC_OK,
	NSS_MC_NO_CORE,
	NSS_MC_IIF,
	NSS_MC_OIF,
	NSS_MC_BRIDGE_VLAN,
	NSS_MC_TOO_MANY,
	NSS_MC_NO_EGRESS,
	NSS_MC_PPPOE,
	NSS_MC_WHY_MAX,
};

static const char *const nss_mc_why_name[] = {
	"ok", "no-core", "iif", "oif", "bridge-vlan", "too-many", "no-egress",
	"pppoe",
};

struct nss_mc_if {
	int ifnum;
	u32 vlan;
	u32 mtu;
	u8 mac[ETH_ALEN];
};

struct nss_mc_in {
	int src;
	u32 vlan;
	u32 mtu;
	u16 sid;
	u8 server[ETH_ALEN];
	u8 local[ETH_ALEN];
	bool pppoe;
};

/* What the installed rules of one (S,G) carry. */
struct nss_mc_set {
	struct nss_mc_in in;
	u8 mac[ETH_ALEN];
	u16 n;
	struct nss_mc_if m[NSS_MC_IF_MAX];
};

struct nss_mc_port {
	__be16 sport;
	__be16 dport;
	bool installed;
};

struct nss_mc_route {
	struct list_head node;
	struct rcu_head rcu;
	struct mr_mfc *mfc;	/* held */
	union nf_inet_addr src;
	union nf_inet_addr grp;
	u8 family;
	int iif;		/* ingress ifindex, for the hook */
	enum nss_mc_why why;
	int pppoe;		/* held session interface, or -1 */
	unsigned long kicked;
	u8 nports;
	struct nss_mc_port port[NSS_MC_PORTS];
	struct nss_mc_set cur;
};

struct nss_mc_event {
	struct list_head node;
	unsigned long type;
	u8 family;
	struct mr_mfc *mfc;	/* held */
	int ifindex;
	u16 vif;
};

/* A bridge port's MDB entry, as switchdev reports it: by MAC. */
struct nss_mc_mdb {
	struct list_head node;
	int ifindex;
	u8 mac[ETH_ALEN];
	u32 ref;
};

#define NSS_MC_FLUSH	(~0UL)

/* The per-interface join and leave flags are shared by both families. */
static_assert(NSS_IPV4_MC_RULE_CREATE_IF_FLAG_JOIN ==
	      NSS_IPV6_MC_RULE_CREATE_IF_FLAG_JOIN);
static_assert(NSS_IPV4_MC_RULE_CREATE_IF_FLAG_LEAVE ==
	      NSS_IPV6_MC_RULE_CREATE_IF_FLAG_LEAVE);

static DEFINE_MUTEX(nss_mc_lock);
static DEFINE_SPINLOCK(nss_mc_ev_lock);
static DEFINE_SPINLOCK(nss_mc_port_lock);
static LIST_HEAD(nss_mc_events);
static LIST_HEAD(nss_mc_routes);
static LIST_HEAD(nss_mc_mdbs);
static struct nss_core *nss_mc_core;
static int nss_mc_vif[2][MAXVIFS];
static unsigned int nss_mc_nroutes;
static bool nss_mc_mdb_lost;	/* an entry went unrecorded: flood instead */
static u32 nss_mc_created, nss_mc_updated, nss_mc_destroyed, nss_mc_refused;
static u32 nss_mc_last_err;

static void nss_mc_work_fn(struct work_struct *work);
static DECLARE_WORK(nss_mc_work, nss_mc_work_fn);

static int nss_mc_fi(u8 family)
{
	return family == RTNL_FAMILY_IP6MR;
}

/* The firmware interface behind @dev, at most one VLAN tag above it,
 * counting a stacked port's conduit tag.
 */
static int nss_mc_port_of(struct nss_core *core, const struct net_device *dev,
			  u32 *vlan, u32 *mtu)
{
	u16 vid = 0, cvid;
	int ifnum;

	if (is_vlan_dev(dev)) {
		*vlan = (u32)ntohs(vlan_dev_vlan_proto(dev)) << 16 |
			vlan_dev_vlan_id(dev);
		vid = vlan_dev_vlan_id(dev);
		dev = vlan_dev_real_dev(dev);
	}

	ifnum = nss_flow_port(core, dev->ifindex, vid, mtu, &cvid);
	if (ifnum >= 0 && cvid) {
		if (vid && *vlan >> 16 != ETH_P_8021Q)
			return -EOPNOTSUPP;
		*vlan = (u32)ETH_P_8021Q << 16 | cvid;
	}

	return ifnum;
}

static enum nss_mc_why nss_mc_ingress(struct nss_core *core,
				      const struct net_device *dev,
				      struct nss_mc_in *in)
{
	in->vlan = NSS_MC_VLAN_NONE;

	/* The session is on the port below; the stack says which. */
	if (dev->type == ARPHRD_PPP) {
		struct net_device_path_ctx ctx = { .dev = dev };
		struct net_device_path_stack st;

		if (dev_fill_forward_path(&ctx, &st) < 0 || st.num_paths < 2 ||
		    st.path[0].type != DEV_PATH_PPPOE)
			return NSS_MC_PPPOE;

		in->pppoe = true;
		in->sid = st.path[0].encap.id;
		ether_addr_copy(in->server, st.path[0].encap.h_dest);
		dev = st.path[1].dev;
		ether_addr_copy(in->local, dev->dev_addr);
	}

	in->src = nss_mc_port_of(core, dev, &in->vlan, &in->mtu);

	return in->src < 0 ? NSS_MC_IIF : NSS_MC_OK;
}

static enum nss_mc_why nss_mc_member(struct nss_core *core,
				     struct nss_mc_set *w,
				     const struct net_device *dev,
				     const struct net_device *l3)
{
	u32 vlan = NSS_MC_VLAN_NONE, mtu;
	struct nss_mc_if *m;
	int i, ifnum;

	ifnum = nss_mc_port_of(core, dev, &vlan, &mtu);
	if (ifnum < 0)
		return NSS_MC_OIF;
	/* Ports of one stacked switch in one bridge share a conduit tag,
	 * and the switch replicates a single copy, as for the bridge.
	 */
	for (i = 0; i < w->n; i++)
		if (w->m[i].ifnum == ifnum && w->m[i].vlan == vlan)
			return NSS_MC_OK;
	if (w->n == NSS_MC_IF_MAX)
		return NSS_MC_TOO_MANY;

	/* Routed out of @l3: its address and MTU, as the stack would. */
	m = &w->m[w->n++];
	m->ifnum = ifnum;
	m->vlan = vlan;
	m->mtu = l3->mtu;
	ether_addr_copy(m->mac, l3->dev_addr);

	return NSS_MC_OK;
}

static bool nss_mc_mdb_has(int ifindex, const u8 *mac)
{
	struct nss_mc_mdb *e;

	list_for_each_entry(e, &nss_mc_mdbs, node)
		if (e->ifindex == ifindex && ether_addr_equal(e->mac, mac))
			return true;

	return false;
}

static bool nss_mc_snooping(struct net_device *br, int proto)
{
	struct net_device *port;
	struct list_head *iter;

	if (nss_mc_mdb_lost || !br_multicast_enabled(br))
		return false;

	/* The query takes any port of the bridge. */
	netdev_for_each_lower_dev(br, port, iter)
		return br_multicast_has_querier_anywhere(port, proto);

	return false;
}

/* The ports br_dev_xmit() would deliver a routed packet for @w's group to.
 * ponytail: mrouter ports are not added; add them if a LAN router appears.
 */
static enum nss_mc_why nss_mc_bridge(struct nss_core *core,
				     struct nss_mc_route *r,
				     struct net_device *br,
				     struct nss_mc_set *w)
{
	int proto = r->family == RTNL_FAMILY_IPMR ? ETH_P_IP : ETH_P_IPV6;
	bool snoop = nss_mc_snooping(br, proto);
	struct net_device *port;
	struct list_head *iter;
	enum nss_mc_why why;

	if (br_vlan_enabled(br))
		return NSS_MC_BRIDGE_VLAN;

	netdev_for_each_lower_dev(br, port, iter) {
		if (br_port_get_stp_state(port) != BR_STATE_FORWARDING)
			continue;
		if (snoop ? !nss_mc_mdb_has(port->ifindex, w->mac) :
			    !br_port_flag_is_set(port, BR_MCAST_FLOOD))
			continue;
		why = nss_mc_member(core, w, port, br);
		if (why)
			return why;
	}

	return NSS_MC_OK;
}

static enum nss_mc_why nss_mc_want(struct nss_core *core,
				   struct nss_mc_route *r,
				   struct nss_mc_set *w)
{
	const struct mr_mfc *c = r->mfc;
	int fi = nss_mc_fi(r->family);
	struct net_device *dev;
	enum nss_mc_why why;
	int v;

	if (c->mfc_parent >= MAXVIFS)
		return NSS_MC_IIF;
	dev = __dev_get_by_index(&init_net, nss_mc_vif[fi][c->mfc_parent]);
	if (!dev)
		return NSS_MC_IIF;
	WRITE_ONCE(r->iif, dev->ifindex);

	if (!core || !core->running)
		return NSS_MC_NO_CORE;

	why = nss_mc_ingress(core, dev, &w->in);
	if (why)
		return why;

	if (r->family == RTNL_FAMILY_IPMR)
		ip_eth_mc_map(r->grp.ip, w->mac);
	else
		ipv6_eth_mc_map(&r->grp.in6, w->mac);

	for (v = c->mfc_un.res.minvif; v < c->mfc_un.res.maxvif; v++) {
		if (v == c->mfc_parent || c->mfc_un.res.ttls[v] == 255)
			continue;
		dev = __dev_get_by_index(&init_net, nss_mc_vif[fi][v]);
		if (!dev)
			return NSS_MC_OIF;
		why = netif_is_bridge_master(dev) ?
		      nss_mc_bridge(core, r, dev, w) :
		      nss_mc_member(core, w, dev, dev);
		if (why)
			return why;
	}

	return w->n ? NSS_MC_OK : NSS_MC_NO_EGRESS;
}

static int nss_mc_send4(struct nss_core *core, struct nss_mc_route *r,
			struct nss_mc_port *p, const struct nss_mc_set *w,
			const struct nss_mc_if *const *m, const u16 *flag,
			int n, bool update)
{
	struct nss_ipv4_mc_rule_create_msg *c;
	struct nss_ipv4_msg *msg __free(kfree) =
		kzalloc(sizeof(*msg), GFP_KERNEL);
	int i, ret;

	if (!msg)
		return -ENOMEM;
	c = &msg->msg.mc_rule_create;

	c->tuple.protocol = IPPROTO_UDP;
	c->tuple.flow_ip = ntohl(r->src.ip);
	c->tuple.flow_ident = ntohs(p->sport);
	c->tuple.return_ip = ntohl(r->grp.ip);
	c->tuple.return_ident = ntohs(p->dport);

	if (update)
		c->rule_flags = NSS_IPV4_MC_RULE_CREATE_FLAG_MC_UPDATE;
	c->src_interface_num = w->in.src;
	c->ingress_vlan_tag[0] = w->in.vlan;
	c->ingress_vlan_tag[1] = NSS_MC_VLAN_NONE;
	if (w->in.vlan != NSS_MC_VLAN_NONE)
		c->valid_flags |= NSS_IPV4_MC_RULE_CREATE_FLAG_INGRESS_VLAN_VALID;
	if (w->in.pppoe)
		c->valid_flags |= NSS_IPV4_MC_RULE_CREATE_FLAG_INGRESS_PPPOE;
	memcpy(c->dest_mac, w->mac, ETH_ALEN);

	c->if_count = n;
	for (i = 0; i < n; i++) {
		struct nss_ipv4_mc_if_rule *ir = &c->if_rule[i];

		ir->rule_flags = NSS_IPV4_MC_RULE_CREATE_IF_FLAG_ROUTED_FLOW |
				 flag[i];
		ir->xlate_src_ip = c->tuple.flow_ip;
		ir->xlate_src_ident = c->tuple.flow_ident;
		ir->egress_vlan_tag[0] = m[i]->vlan;
		ir->egress_vlan_tag[1] = NSS_MC_VLAN_NONE;
		if (m[i]->vlan != NSS_MC_VLAN_NONE)
			ir->valid_flags |= NSS_IPV4_MC_RULE_CREATE_IF_FLAG_VLAN_VALID;
		ir->if_num = m[i]->ifnum;
		ir->if_mtu = m[i]->mtu;
		memcpy(ir->if_mac, m[i]->mac, ETH_ALEN);
	}

	msg->cm.interface = NSS_INTERFACE_IPV4;
	msg->cm.type = NSS_IPV4_TX_CREATE_MC_RULE_MSG;
	ret = nss_msg_send(core, msg, offsetof(struct nss_ipv4_msg, msg) +
				      sizeof(*c));
	if (msg->cm.error) {
		nss_mc_last_err = msg->cm.error;
		if (!ret)
			ret = -EIO;
	}

	return ret;
}

static int nss_mc_send6(struct nss_core *core, struct nss_mc_route *r,
			struct nss_mc_port *p, const struct nss_mc_set *w,
			const struct nss_mc_if *const *m, const u16 *flag,
			int n, bool update)
{
	struct nss_ipv6_mc_rule_create_msg *c;
	struct nss_ipv6_msg *msg __free(kfree) =
		kzalloc(sizeof(*msg), GFP_KERNEL);
	int i, ret;

	if (!msg)
		return -ENOMEM;
	c = &msg->msg.mc_rule_create;

	c->tuple.protocol = IPPROTO_UDP;
	for (i = 0; i < 4; i++) {
		c->tuple.flow_ip[i] = ntohl(r->src.ip6[i]);
		c->tuple.return_ip[i] = ntohl(r->grp.ip6[i]);
	}
	c->tuple.flow_ident = ntohs(p->sport);
	c->tuple.return_ident = ntohs(p->dport);

	if (update)
		c->rule_flags = NSS_IPV6_MC_RULE_CREATE_FLAG_MC_UPDATE;
	c->src_interface_num = w->in.src;
	c->ingress_vlan_tag[0] = w->in.vlan;
	c->ingress_vlan_tag[1] = NSS_MC_VLAN_NONE;
	if (w->in.vlan != NSS_MC_VLAN_NONE)
		c->valid_flags |= NSS_IPV6_MC_RULE_CREATE_FLAG_INGRESS_VLAN_VALID;
	if (w->in.pppoe)
		c->valid_flags |= NSS_IPV6_MC_RULE_CREATE_FLAG_INGRESS_PPPOE;
	memcpy(c->dest_mac, w->mac, ETH_ALEN);

	c->if_count = n;
	for (i = 0; i < n; i++) {
		struct nss_ipv6_mc_if_rule *ir = &c->if_rule[i];

		ir->rule_flags = NSS_IPV6_MC_RULE_CREATE_IF_FLAG_ROUTED_FLOW |
				 flag[i];
		ir->egress_vlan_tag[0] = m[i]->vlan;
		ir->egress_vlan_tag[1] = NSS_MC_VLAN_NONE;
		if (m[i]->vlan != NSS_MC_VLAN_NONE)
			ir->valid_flags |= NSS_IPV6_MC_RULE_CREATE_IF_FLAG_VLAN_VALID;
		ir->if_num = m[i]->ifnum;
		ir->if_mtu = m[i]->mtu;
		memcpy(ir->if_mac, m[i]->mac, ETH_ALEN);
	}

	msg->cm.interface = NSS_INTERFACE_IPV6;
	msg->cm.type = NSS_IPV6_TX_CREATE_MC_RULE_MSG;
	ret = nss_msg_send(core, msg, offsetof(struct nss_ipv6_msg, msg) +
				      sizeof(*c));
	if (msg->cm.error) {
		nss_mc_last_err = msg->cm.error;
		if (!ret)
			ret = -EIO;
	}

	return ret;
}

static int nss_mc_send(struct nss_core *core, struct nss_mc_route *r,
		       struct nss_mc_port *p, const struct nss_mc_set *w,
		       const struct nss_mc_if *const *m, const u16 *flag,
		       int n, bool update)
{
	int ret;

	ret = r->family == RTNL_FAMILY_IPMR ?
	      nss_mc_send4(core, r, p, w, m, flag, n, update) :
	      nss_mc_send6(core, r, p, w, m, flag, n, update);
	if (ret)
		nss_mc_refused++;

	return ret;
}

static int nss_mc_create(struct nss_core *core, struct nss_mc_route *r,
			 struct nss_mc_port *p, const struct nss_mc_set *w)
{
	const struct nss_mc_if *m[NSS_MC_IF_MAX];
	u16 flag[NSS_MC_IF_MAX];
	int i, ret;

	for (i = 0; i < w->n; i++) {
		m[i] = &w->m[i];
		flag[i] = NSS_IPV4_MC_RULE_CREATE_IF_FLAG_JOIN;
	}

	ret = nss_mc_send(core, r, p, w, m, flag, w->n, false);
	if (!ret)
		nss_mc_created++;

	return ret;
}

static void nss_mc_destroy(struct nss_core *core, struct nss_mc_route *r,
			   struct nss_mc_port *p)
{
	int i;

	if (r->family == RTNL_FAMILY_IPMR) {
		struct nss_ipv4_msg *m __free(kfree) =
			kzalloc(sizeof(*m), GFP_KERNEL);

		if (!m)
			return;
		m->msg.rule_destroy.tuple.protocol = IPPROTO_UDP;
		m->msg.rule_destroy.tuple.flow_ip = ntohl(r->src.ip);
		m->msg.rule_destroy.tuple.flow_ident = ntohs(p->sport);
		m->msg.rule_destroy.tuple.return_ip = ntohl(r->grp.ip);
		m->msg.rule_destroy.tuple.return_ident = ntohs(p->dport);
		m->cm.interface = NSS_INTERFACE_IPV4;
		m->cm.type = NSS_IPV4_TX_DESTROY_RULE_MSG;
		nss_msg_send(core, m, offsetof(struct nss_ipv4_msg, msg) +
				      sizeof(m->msg.rule_destroy));
	} else {
		struct nss_ipv6_msg *m __free(kfree) =
			kzalloc(sizeof(*m), GFP_KERNEL);

		if (!m)
			return;
		m->msg.rule_destroy.tuple.protocol = IPPROTO_UDP;
		for (i = 0; i < 4; i++) {
			m->msg.rule_destroy.tuple.flow_ip[i] =
				ntohl(r->src.ip6[i]);
			m->msg.rule_destroy.tuple.return_ip[i] =
				ntohl(r->grp.ip6[i]);
		}
		m->msg.rule_destroy.tuple.flow_ident = ntohs(p->sport);
		m->msg.rule_destroy.tuple.return_ident = ntohs(p->dport);
		m->cm.interface = NSS_INTERFACE_IPV6;
		m->cm.type = NSS_IPV6_TX_DESTROY_RULE_MSG;
		nss_msg_send(core, m, offsetof(struct nss_ipv6_msg, msg) +
				      sizeof(m->msg.rule_destroy));
	}

	nss_mc_destroyed++;
}

static bool nss_mc_in_set(const struct nss_mc_set *s, const struct nss_mc_if *m)
{
	int i;

	for (i = 0; i < s->n; i++)
		if (!memcmp(&s->m[i], m, sizeof(*m)))
			return true;

	return false;
}

/* Move an installed rule from r->cur to @w: leaves first, then joins. A
 * failed update leaves the rule in an unknown state.
 */
static int nss_mc_update(struct nss_core *core, struct nss_mc_route *r,
			 struct nss_mc_port *p, const struct nss_mc_set *w)
{
	const struct nss_mc_if *m[NSS_MC_IF_MAX];
	u16 flag[NSS_MC_IF_MAX];
	int i, n = 0, ret;

	for (i = 0; i < r->cur.n; i++) {
		if (nss_mc_in_set(w, &r->cur.m[i]))
			continue;
		if (n == NSS_MC_IF_MAX)
			return -E2BIG;
		m[n] = &r->cur.m[i];
		flag[n++] = NSS_IPV4_MC_RULE_CREATE_IF_FLAG_LEAVE;
	}
	for (i = 0; i < w->n; i++) {
		if (nss_mc_in_set(&r->cur, &w->m[i]))
			continue;
		if (n == NSS_MC_IF_MAX)
			return -E2BIG;
		m[n] = &w->m[i];
		flag[n++] = NSS_IPV4_MC_RULE_CREATE_IF_FLAG_JOIN;
	}
	if (!n)
		return 0;

	ret = nss_mc_send(core, r, p, w, m, flag, n, true);
	if (!ret)
		nss_mc_updated++;

	return ret;
}

static void nss_mc_mark(struct nss_mc_route *r, bool on)
{
	if (on)
		r->mfc->mfc_flags |= MFC_OFFLOAD;
	else
		r->mfc->mfc_flags &= ~MFC_OFFLOAD;
}

/* Take every rule of @r out of the firmware, and the session with them. */
static void nss_mc_withdraw(struct nss_core *core, struct nss_mc_route *r)
{
	/* Pairs with the release in nss_mc_learn(). */
	int i, n = smp_load_acquire(&r->nports);

	for (i = 0; i < n; i++) {
		if (r->port[i].installed && core)
			nss_mc_destroy(core, r, &r->port[i]);
		WRITE_ONCE(r->port[i].installed, false);
	}
	if (r->pppoe >= 0 && core)
		nss_flow_pppoe_drop(core, r->pppoe);
	r->pppoe = -1;
	memset(&r->cur, 0, sizeof(r->cur));
	nss_mc_mark(r, false);
}

static void nss_mc_sync(struct nss_core *core, struct nss_mc_route *r)
{
	struct nss_mc_set *w __free(kfree) = kzalloc(sizeof(*w), GFP_KERNEL);
	/* Pairs with the release in nss_mc_learn(). */
	int i, n = smp_load_acquire(&r->nports);
	bool any = false;

	if (!w)
		return;

	r->why = nss_mc_want(core, r, w);
	if (r->why || memcmp(&w->in, &r->cur.in, sizeof(w->in)) ||
	    !ether_addr_equal(w->mac, r->cur.mac))
		nss_mc_withdraw(core, r);
	if (r->why || !n)
		return;

	if (w->in.pppoe && r->pppoe < 0) {
		r->pppoe = nss_flow_pppoe_hold(core, w->in.sid, w->in.server,
					       w->in.local, w->in.src,
					       w->in.mtu,
					       w->in.vlan == NSS_MC_VLAN_NONE ?
					       0 : w->in.vlan);
		if (r->pppoe < 0) {
			r->pppoe = -1;
			r->why = NSS_MC_PPPOE;
			return;
		}
	}

	for (i = 0; i < n; i++) {
		struct nss_mc_port *p = &r->port[i];

		if (p->installed && nss_mc_update(core, r, p, w)) {
			nss_mc_destroy(core, r, p);
			WRITE_ONCE(p->installed, false);
		}
		if (!p->installed)
			WRITE_ONCE(p->installed, !nss_mc_create(core, r, p, w));
		any |= p->installed;
	}

	r->cur = *w;
	nss_mc_mark(r, any);
}

static struct nss_mc_route *nss_mc_find(const struct mr_mfc *mfc)
{
	struct nss_mc_route *r;

	list_for_each_entry(r, &nss_mc_routes, node)
		if (r->mfc == mfc)
			return r;

	return NULL;
}

static void nss_mc_route_add(struct nss_mc_event *ev)
{
	struct nss_mc_route *r;

	r = kzalloc(sizeof(*r), GFP_KERNEL);
	if (!r)
		return;

	if (ev->family == RTNL_FAMILY_IPMR) {
		struct mfc_cache *c = container_of(ev->mfc, struct mfc_cache, _c);

		r->src.ip = c->mfc_origin;
		r->grp.ip = c->mfc_mcastgrp;
	} else {
		struct mfc6_cache *c = container_of(ev->mfc, struct mfc6_cache,
						    _c);

		r->src.in6 = c->mf6c_origin;
		r->grp.in6 = c->mf6c_mcastgrp;
	}

	/* ponytail: (*,G) is not offloaded; the rule needs a source */
	if (nf_inet_addr_cmp(&r->src, &(union nf_inet_addr){})) {
		kfree(r);
		return;
	}

	r->family = ev->family;
	r->mfc = ev->mfc;
	r->pppoe = -1;
	ev->mfc = NULL;
	list_add_tail_rcu(&r->node, &nss_mc_routes);
	WRITE_ONCE(nss_mc_nroutes, nss_mc_nroutes + 1);
}

static void nss_mc_route_del(struct nss_core *core, struct nss_mc_route *r)
{
	nss_mc_withdraw(core, r);
	list_del_rcu(&r->node);
	WRITE_ONCE(nss_mc_nroutes, nss_mc_nroutes - 1);
	mr_cache_put(r->mfc);
	kfree_rcu(r, rcu);
}

static void nss_mc_event_run(struct nss_core *core, struct nss_mc_event *ev)
{
	struct nss_mc_route *r, *tmp;
	int fi = nss_mc_fi(ev->family);

	switch (ev->type) {
	case NSS_MC_FLUSH:
		list_for_each_entry_safe(r, tmp, &nss_mc_routes, node)
			nss_mc_route_del(core, r);
		memset(nss_mc_vif, 0, sizeof(nss_mc_vif));
		break;
	case FIB_EVENT_VIF_ADD:
		nss_mc_vif[fi][ev->vif] = ev->ifindex;
		break;
	case FIB_EVENT_VIF_DEL:
		nss_mc_vif[fi][ev->vif] = 0;
		break;
	case FIB_EVENT_ENTRY_ADD:
	case FIB_EVENT_ENTRY_REPLACE:
		if (!nss_mc_find(ev->mfc))
			nss_mc_route_add(ev);
		break;
	case FIB_EVENT_ENTRY_DEL:
		r = nss_mc_find(ev->mfc);
		if (r)
			nss_mc_route_del(core, r);
		break;
	}
}

static void nss_mc_work_fn(struct work_struct *work)
{
	struct nss_mc_event *ev, *tmp;
	struct nss_mc_route *r;
	struct nss_core *core;
	LIST_HEAD(events);

	spin_lock_bh(&nss_mc_ev_lock);
	list_splice_init(&nss_mc_events, &events);
	spin_unlock_bh(&nss_mc_ev_lock);

	rtnl_lock();
	mutex_lock(&nss_mc_lock);

	core = nss_mc_core;
	list_for_each_entry_safe(ev, tmp, &events, node) {
		nss_mc_event_run(core, ev);
		if (ev->mfc)
			mr_cache_put(ev->mfc);
		kfree(ev);
	}

	list_for_each_entry(r, &nss_mc_routes, node)
		nss_mc_sync(core, r);

	mutex_unlock(&nss_mc_lock);
	rtnl_unlock();
}

static void nss_mc_queue(struct nss_mc_event *ev)
{
	spin_lock_bh(&nss_mc_ev_lock);
	list_add_tail(&ev->node, &nss_mc_events);
	spin_unlock_bh(&nss_mc_ev_lock);
	schedule_work(&nss_mc_work);
}

/* Atomic context: queue a copy, holding the MFC entry until it is read. */
static int nss_mc_fib_event(struct notifier_block *nb, unsigned long type,
			    void *ptr)
{
	struct fib_notifier_info *info = ptr;
	struct mfc_entry_notifier_info *mi;
	struct vif_entry_notifier_info *vi;
	struct nss_mc_event *ev;
	u32 tb;

	if (info->family != RTNL_FAMILY_IPMR &&
	    info->family != RTNL_FAMILY_IP6MR)
		return NOTIFY_DONE;
	tb = info->family == RTNL_FAMILY_IPMR ? RT_TABLE_DEFAULT :
						 RT6_TABLE_DFLT;

	switch (type) {
	case FIB_EVENT_ENTRY_ADD:
	case FIB_EVENT_ENTRY_REPLACE:
	case FIB_EVENT_ENTRY_DEL:
		mi = container_of(info, struct mfc_entry_notifier_info, info);
		if (mi->tb_id != tb)
			return NOTIFY_DONE;
		ev = kzalloc(sizeof(*ev), GFP_ATOMIC);
		if (!ev)
			return notifier_from_errno(-ENOMEM);
		mr_cache_hold(mi->mfc);
		ev->mfc = mi->mfc;
		break;
	case FIB_EVENT_VIF_ADD:
	case FIB_EVENT_VIF_DEL:
		vi = container_of(info, struct vif_entry_notifier_info, info);
		if (vi->tb_id != tb || vi->vif_index >= MAXVIFS)
			return NOTIFY_DONE;
		ev = kzalloc(sizeof(*ev), GFP_ATOMIC);
		if (!ev)
			return notifier_from_errno(-ENOMEM);
		ev->vif = vi->vif_index;
		ev->ifindex = vi->dev ? vi->dev->ifindex : 0;
		break;
	default:
		return NOTIFY_DONE;
	}

	ev->type = type;
	ev->family = info->family;
	nss_mc_queue(ev);

	return NOTIFY_DONE;
}

/* A dump that raced a change is repeated; forget what it delivered. */
static void nss_mc_fib_flush(struct notifier_block *nb)
{
	struct nss_mc_event *ev = kzalloc(sizeof(*ev), GFP_KERNEL);

	if (!ev)
		return;
	ev->type = NSS_MC_FLUSH;
	nss_mc_queue(ev);
}

static struct notifier_block nss_mc_fib_nb = {
	.notifier_call = nss_mc_fib_event,
};

static void nss_mc_mdb_note(int ifindex, const u8 *mac, bool add)
{
	struct nss_mc_mdb *e;

	guard(mutex)(&nss_mc_lock);

	list_for_each_entry(e, &nss_mc_mdbs, node) {
		if (e->ifindex != ifindex || !ether_addr_equal(e->mac, mac))
			continue;
		if (add) {
			e->ref++;
		} else if (!--e->ref) {
			list_del(&e->node);
			kfree(e);
		}
		return;
	}
	if (!add)
		return;

	e = kzalloc(sizeof(*e), GFP_KERNEL);
	if (!e) {
		nss_mc_mdb_lost = true;
		return;
	}
	e->ifindex = ifindex;
	ether_addr_copy(e->mac, mac);
	e->ref = 1;
	list_add(&e->node, &nss_mc_mdbs);
}

/* The bridge reports MDB and port-state changes for every port, offloaded
 * or not. NOTIFY_DONE: this only watches.
 */
static int nss_mc_switchdev_event(struct notifier_block *nb,
				  unsigned long event, void *ptr)
{
	struct switchdev_notifier_port_attr_info *ai = ptr;
	struct switchdev_notifier_port_obj_info *oi = ptr;
	struct net_device *dev = switchdev_notifier_info_to_dev(ptr);

	switch (event) {
	case SWITCHDEV_PORT_OBJ_ADD:
	case SWITCHDEV_PORT_OBJ_DEL:
		if (oi->obj->id != SWITCHDEV_OBJ_ID_PORT_MDB)
			return NOTIFY_DONE;
		nss_mc_mdb_note(dev->ifindex,
				SWITCHDEV_OBJ_PORT_MDB(oi->obj)->addr,
				event == SWITCHDEV_PORT_OBJ_ADD);
		break;
	case SWITCHDEV_PORT_ATTR_SET:
		if (ai->attr->id != SWITCHDEV_ATTR_ID_PORT_STP_STATE &&
		    ai->attr->id != SWITCHDEV_ATTR_ID_BRIDGE_MC_DISABLED)
			return NOTIFY_DONE;
		break;
	default:
		return NOTIFY_DONE;
	}

	if (READ_ONCE(nss_mc_nroutes))
		schedule_work(&nss_mc_work);

	return NOTIFY_DONE;
}

static struct notifier_block nss_mc_switchdev_nb = {
	.notifier_call = nss_mc_switchdev_event,
};

/* Record the ports of a known (S,G) arriving on its ingress. A pair already
 * known but not installed retries at most once a second, which is also how
 * a route is picked up once the core runs or a port is armed.
 */
static void nss_mc_learn(u8 family, const union nf_inet_addr *s,
			 const union nf_inet_addr *g, int ifindex,
			 __be16 sport, __be16 dport)
{
	struct nss_mc_route *r;
	bool kick = false;
	int i;

	list_for_each_entry_rcu(r, &nss_mc_routes, node) {
		if (r->family != family || READ_ONCE(r->iif) != ifindex ||
		    !nf_inet_addr_cmp(&r->src, s) ||
		    !nf_inet_addr_cmp(&r->grp, g))
			continue;

		spin_lock_bh(&nss_mc_port_lock);
		for (i = 0; i < r->nports; i++)
			if (r->port[i].sport == sport &&
			    r->port[i].dport == dport)
				break;
		if (i == r->nports && i < NSS_MC_PORTS) {
			r->port[i].sport = sport;
			r->port[i].dport = dport;
			/* The work reads the pair once it sees the count. */
			smp_store_release(&r->nports, i + 1);
			kick = true;
		} else if (i < r->nports && !READ_ONCE(r->port[i].installed) &&
			   time_after(jiffies, r->kicked + HZ)) {
			r->kicked = jiffies;
			kick = true;
		}
		spin_unlock_bh(&nss_mc_port_lock);
		break;
	}

	if (kick)
		schedule_work(&nss_mc_work);
}

static unsigned int nss_mc_hook(void *priv, struct sk_buff *skb,
				const struct nf_hook_state *state)
{
	union nf_inet_addr s = {}, g = {};
	struct udphdr _uh;
	const struct udphdr *uh;
	unsigned int thoff;
	u8 family;

	if (!READ_ONCE(nss_mc_nroutes))
		return NF_ACCEPT;

	if (state->pf == NFPROTO_IPV4) {
		const struct iphdr *iph = ip_hdr(skb);

		if (!ipv4_is_multicast(iph->daddr) ||
		    iph->protocol != IPPROTO_UDP || ip_is_fragment(iph))
			return NF_ACCEPT;
		s.ip = iph->saddr;
		g.ip = iph->daddr;
		thoff = ip_hdrlen(skb);
		family = RTNL_FAMILY_IPMR;
	} else {
		const struct ipv6hdr *ip6h = ipv6_hdr(skb);

		/* ponytail: no extension headers before UDP */
		if (!ipv6_addr_is_multicast(&ip6h->daddr) ||
		    ip6h->nexthdr != IPPROTO_UDP)
			return NF_ACCEPT;
		s.in6 = ip6h->saddr;
		g.in6 = ip6h->daddr;
		thoff = sizeof(*ip6h);
		family = RTNL_FAMILY_IP6MR;
	}

	uh = skb_header_pointer(skb, skb_network_offset(skb) + thoff,
				sizeof(_uh), &_uh);
	if (uh)
		nss_mc_learn(family, &s, &g, state->in->ifindex, uh->source,
			     uh->dest);

	return NF_ACCEPT;
}

static struct nf_hook_ops nss_mc_hook_ops[] = {
	{
		.hook		= nss_mc_hook,
		.pf		= NFPROTO_IPV4,
		.hooknum	= NF_INET_PRE_ROUTING,
		.priority	= NF_IP_PRI_CONNTRACK + 1,
	},
	{
		.hook		= nss_mc_hook,
		.pf		= NFPROTO_IPV6,
		.hooknum	= NF_INET_PRE_ROUTING,
		.priority	= NF_IP6_PRI_CONNTRACK + 1,
	},
};

/* Without routes the hook returns at once, so only IPv6 itself gates it. */
#define NSS_MC_NHOOKS	(IS_ENABLED(CONFIG_IPV6) ? 2 : 1)

void nss_mc_bind(struct nss_core *core)
{
	guard(mutex)(&nss_mc_lock);
	nss_mc_core = core;
}

/* A halted core holds no rules and no sessions; nothing is sent. */
void nss_mc_flush(void)
{
	struct nss_mc_route *r;

	guard(mutex)(&nss_mc_lock);

	list_for_each_entry(r, &nss_mc_routes, node) {
		nss_mc_withdraw(NULL, r);
		r->why = NSS_MC_NO_CORE;
	}
}

void nss_mc_print(struct seq_file *s)
{
	struct nss_mc_route *r;
	struct nss_mc_mdb *e;
	int i;

	guard(mutex)(&nss_mc_lock);

	seq_printf(s, "routes %u created %u updated %u destroyed %u refused %u last-error %u mdb-lost %d\n",
		   nss_mc_nroutes, nss_mc_created, nss_mc_updated,
		   nss_mc_destroyed, nss_mc_refused, nss_mc_last_err,
		   nss_mc_mdb_lost);

	list_for_each_entry(e, &nss_mc_mdbs, node)
		seq_printf(s, "mdb ifindex %d %pM ref %u\n", e->ifindex, e->mac,
			   e->ref);

	list_for_each_entry(r, &nss_mc_routes, node) {
		if (r->family == RTNL_FAMILY_IPMR)
			seq_printf(s, "route %pI4 -> %pI4", &r->src.ip,
				   &r->grp.ip);
		else
			seq_printf(s, "route %pI6c -> %pI6c", &r->src.in6,
				   &r->grp.in6);
		seq_printf(s, " iif %d %s src %d vlan %#x pppoe %d offload %d\n",
			   r->iif, nss_mc_why_name[r->why], r->cur.in.src,
			   r->cur.in.vlan, r->pppoe,
			   !!(r->mfc->mfc_flags & MFC_OFFLOAD));
		for (i = 0; i < r->nports; i++)
			seq_printf(s, "  ports %u -> %u installed %d\n",
				   ntohs(r->port[i].sport),
				   ntohs(r->port[i].dport),
				   r->port[i].installed);
		for (i = 0; i < r->cur.n; i++)
			seq_printf(s, "  if %d vlan %#x mtu %u %pM\n",
				   r->cur.m[i].ifnum, r->cur.m[i].vlan,
				   r->cur.m[i].mtu, r->cur.m[i].mac);
	}
}

static void nss_mc_drain(void)
{
	struct nss_mc_event *ev, *etmp;
	struct nss_mc_route *r, *rtmp;
	struct nss_mc_mdb *e, *mtmp;

	cancel_work_sync(&nss_mc_work);

	list_for_each_entry_safe(ev, etmp, &nss_mc_events, node) {
		if (ev->mfc)
			mr_cache_put(ev->mfc);
		kfree(ev);
	}
	INIT_LIST_HEAD(&nss_mc_events);

	rtnl_lock();
	list_for_each_entry_safe(r, rtmp, &nss_mc_routes, node)
		nss_mc_route_del(NULL, r);
	rtnl_unlock();

	list_for_each_entry_safe(e, mtmp, &nss_mc_mdbs, node)
		kfree(e);
	INIT_LIST_HEAD(&nss_mc_mdbs);
}

int nss_mc_init(void)
{
	int ret;

	ret = register_switchdev_blocking_notifier(&nss_mc_switchdev_nb);
	if (ret)
		return ret;

	ret = nf_register_net_hooks(&init_net, nss_mc_hook_ops, NSS_MC_NHOOKS);
	if (ret)
		goto switchdev;

	ret = register_fib_notifier(&init_net, &nss_mc_fib_nb,
				    nss_mc_fib_flush, NULL);
	if (ret)
		goto hooks;

	return 0;

hooks:
	nf_unregister_net_hooks(&init_net, nss_mc_hook_ops, NSS_MC_NHOOKS);
switchdev:
	unregister_switchdev_blocking_notifier(&nss_mc_switchdev_nb);
	nss_mc_drain();

	return ret;
}

/* The core is already unbound and halted, so nothing is sent. */
void nss_mc_exit(void)
{
	unregister_fib_notifier(&init_net, &nss_mc_fib_nb);
	nf_unregister_net_hooks(&init_net, nss_mc_hook_ops, NSS_MC_NHOOKS);
	unregister_switchdev_blocking_notifier(&nss_mc_switchdev_nb);
	nss_mc_drain();
}
