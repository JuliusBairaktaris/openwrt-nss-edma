// SPDX-License-Identifier: GPL-2.0-only
/*
 * qca_ppe_nss.c
 *	NSS data-plane glue between qca-nss-drv and the qca_edma/qca_ppe
 *	ethernet stack (DSA user ports).
 *
 * Phase 2b: route individual DSA user ports through the NSS firmware
 * slow path. A port mask written to debugfs qca-ppe-nss/fw_mask "arms"
 * ports BEFORE qca-nss-drv is loaded; when the firmware boots, nss-drv
 * runs its one-shot registration (workqueue, once per firmware boot)
 * and for each armed port:
 *
 *   nss_dp_get_netdev_by_nss_if_num()  -> we expose the user netdev
 *   nss_dp_override_data_plane()       -> we record nss-drv's dp_ops
 *                                         (xmit feeds the H2N ring) and
 *                                         run init (adds 32B preheader
 *                                         headroom to the netdev)
 *   nss_dp_start_data_plane()          -> we replay the old nss-dp
 *                                         ndo_open bring-up against the
 *                                         firmware: vsi_assign (a VSI
 *                                         private to the port, allocated
 *                                         by qca_ppe, the single PPE
 *                                         table writer - the fw NACKs
 *                                         sharing a VSI across ports),
 *                                         mac_addr, change_mtu, open,
 *                                         link_state; then claim the
 *                                         port's conduit TX via
 *                                         qca_edma's redirect hook so
 *                                         dp_ops->xmit carries all of
 *                                         the port's traffic.
 *
 * RX: the firmware EDMA owns all PPE CPU-port delivery once booted
 * (measured: host ring 15 is unmapped from every queue). nss-drv hands
 * us every packet/exception via nss_dp_receive(); we inject it into
 * the user port exactly like the DSA OOB host receive path would
 * (the conduit legitimately never saw the frame).
 *
 * Lifecycle after start: the port is registered as the qca-edma
 * datapath owner, so the driver synchronously revokes injection (fw
 * link_state 0) before every port transition and re-grants (link_state
 * 1) once link-up completes. The netdev notifier handles the rest:
 * DOWN closes the firmware port and releases the claim, UP re-opens,
 * CHANGEMTU/CHANGEADDR re-send the respective firmware messages
 * (bridge moves need no fw action: the fw VSI is port-private, and
 * the Linux bridge owns all forwarding).
 * Changing the armed mask while a port is overridden is refused:
 * nss-drv registration runs once per firmware boot, so changing the
 * mask means reloading qca-nss-drv.
 *
 * PPE port id == DSA user port index == OOB tag port == NSS phys_if
 * number (1..6); the CPU port 0 is the qca-edma conduit.
 *
 * Lock order everywhere: rtnl -> ppe_nss_lock.
 */

#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/debugfs.h>
#include <linux/phy.h>
#include <linux/clk.h>
#include <linux/of.h>
#include <linux/rtnetlink.h>
#include <linux/etherdevice.h>
#include <linux/uaccess.h>
#include <linux/soc/qcom/qca_edma.h>
#include <linux/soc/qcom/qca_ppe.h>
#include <net/dsa.h>
#include <nss_dp_api_if.h>

enum ppe_nss_port_state {
	PPE_NSS_PORT_IDLE,		/* free slot */
	PPE_NSS_PORT_ARMED,		/* netdevs held, exposed to nss-drv at registration */
	PPE_NSS_PORT_OVERRIDDEN,	/* nss-drv dp_ops recorded for this fw boot */
	PPE_NSS_PORT_STARTED,		/* fw port open, conduit datapath claimed */
};

struct ppe_nss_port {
	int if_num;
	struct net_device *netdev;	/* DSA user port, held while armed */
	struct net_device *conduit;	/* qca-edma conduit, held while armed */
	enum ppe_nss_port_state state;
	bool headroom_added;		/* dp_ops->init() added 32B to needed_headroom */
	int fw_vsi;			/* VSI currently assigned to fw, -1 = none */
	struct nss_dp_data_plane_ops *dp_ops;
	struct nss_dp_data_plane_ctx *dpc;
	atomic64_t tx_redirect_pkts;
	atomic64_t tx_busy;
	atomic64_t rx_fw_pkts;
	/*
	 * What we last told the firmware about this port's link, and whether it
	 * took it. The firmware's own idea of a port's link state is not
	 * readable anywhere - there is no gmac/phys_if stats node on ipq807x -
	 * and it is only ever a shadow of our revoke/grant edges, so a port that
	 * is dead while this says "up, no error" puts the divergence inside the
	 * firmware, and one that says "down" names a missed edge on our side.
	 * Without this a wedged port cannot be attributed to either.
	 */
	u8 fw_link_up;
	u8 fw_link_failed;
	atomic_t fw_link_changes;
	atomic_t fw_link_skipped;
};

static struct ppe_nss_port ppe_nss_ports[NSS_DP_MAX_INTERFACES];
static DEFINE_MUTEX(ppe_nss_lock);
static unsigned long ppe_nss_fw_mask;
static struct dentry *ppe_nss_dentry;
static atomic_t ppe_nss_rx_unexpected = ATOMIC_INIT(0);

/*
 * Resolve the DSA user port with the given port index on a qca-edma
 * conduit. Caller holds rtnl.
 */
static struct net_device *ppe_nss_find_user_port(int if_num,
						 struct net_device **conduitp)
{
	struct net_device *dev;

	for_each_netdev(&init_net, dev) {
		struct net_device *conduit;
		struct dsa_port *dp;

		if (!dsa_user_dev_check(dev))
			continue;

		dp = dsa_port_from_netdev(dev);
		if (IS_ERR(dp) || dp->index != if_num)
			continue;

		conduit = dsa_port_to_conduit(dp);
		if (!qca_edma_netdev_is_conduit(conduit))
			continue;

		*conduitp = conduit;
		return dev;
	}

	return NULL;
}

/*
 * Take/drop the netdev pair backing a port slot. Callers hold rtnl
 * (netdev iteration) and ppe_nss_lock.
 */
static int ppe_nss_port_hold(struct ppe_nss_port *port, int if_num)
{
	struct net_device *dev, *conduit = NULL;

	ASSERT_RTNL();

	dev = ppe_nss_find_user_port(if_num, &conduit);
	if (!dev) {
		pr_warn("qca-ppe-nss: no DSA user port with index %d\n",
			if_num);
		return -ENODEV;
	}

	dev_hold(dev);
	dev_hold(conduit);
	port->if_num = if_num;
	port->netdev = dev;
	port->conduit = conduit;
	return 0;
}

static void ppe_nss_port_put(struct ppe_nss_port *port)
{
	dev_put(port->netdev);
	dev_put(port->conduit);
	port->netdev = NULL;
	port->conduit = NULL;
}

/*
 * ===== firmware data plane (phase 2b) =====
 */

/*
 * Conduit TX claimed for an fw-started port: hand the frame to
 * nss-drv's xmit (H2N ring; it grows headroom for the 32B preheader
 * itself and maps a full queue to NETDEV_TX_BUSY for requeue).
 *
 * Runs in the conduit's ndo_start_xmit (BH, RCU read side). dp_ops/dpc
 * are stable here: set before the datapath is claimed, cleared only
 * after qca_edma_port_dp_release() guarantees no handler is in flight.
 */
static netdev_tx_t ppe_nss_fw_xmit(struct sk_buff *skb, void *ctx)
{
	struct ppe_nss_port *port = ctx;
	netdev_tx_t ret;

	ret = port->dp_ops->xmit(port->dpc, skb);
	if (unlikely(ret == NETDEV_TX_BUSY))
		atomic64_inc(&port->tx_busy);
	else
		atomic64_inc(&port->tx_redirect_pkts);
	return ret;
}

/*
 * Transition revoke/grant, called by qca-edma around every port
 * transition (process context, owner lock held - the claim cannot be
 * released concurrently, so dp_ops/dpc are stable). The fw stops/
 * resumes forwarding accelerated flows to the port; conduit TX for the
 * port is already suspended/resumed by the driver itself. Must not
 * call back into the claim/release API or take ppe_nss_lock.
 */
static int ppe_nss_fw_link_state(struct ppe_nss_port *port, bool up)
{
	int err;

	/*
	 * The firmware's view of a port changes only when we change it, so a
	 * notification restating what it already acknowledged buys nothing and
	 * costs a synchronous round trip in a path held under rtnl. Skip it -
	 * unless the last attempt failed, which leaves its view unknown and the
	 * next attempt obliged to find out.
	 *
	 * This is the discipline nss-dp applies to the same firmware call: it
	 * keeps a link-state shadow and returns early unless the state actually
	 * differs (nss_dp_main.c, nss_dp_adjust_link). Driving the call from
	 * every port transition instead of every link change is why this stack
	 * issued several times as many of them.
	 */
	if (port->fw_link_up == up && !port->fw_link_failed) {
		atomic_inc(&port->fw_link_skipped);
		return 0;
	}

	err = port->dp_ops->link_state(port->dpc, up ? 1 : 0);

	if (err)
		netdev_warn(port->netdev, "qca-ppe-nss: fw link %s notify failed\n",
			    up ? "up" : "down");

	/*
	 * Track what the firmware acknowledged, not what we asked of it. A
	 * refused notify leaves the firmware's idea of the port unchanged, and
	 * recording our intent instead would assert a link the firmware will
	 * not transmit on.
	 */
	if (!err)
		port->fw_link_up = up;
	port->fw_link_failed = !!err;
	atomic_inc(&port->fw_link_changes);

	return err;
}

static void ppe_nss_fw_revoke(void *ctx)
{
	ppe_nss_fw_link_state(ctx, false);
}

static int ppe_nss_fw_grant(void *ctx)
{
	return ppe_nss_fw_link_state(ctx, true);
}

static const struct qca_edma_dp_owner ppe_nss_dp_owner = {
	.xmit	= ppe_nss_fw_xmit,
	.revoke	= ppe_nss_fw_revoke,
	.grant	= ppe_nss_fw_grant,
};

/*
 * Bring the firmware data plane up for an overridden port, replaying
 * the old nss-dp ndo_open sequence, then claim conduit TX. Caller
 * holds rtnl and ppe_nss_lock; nss-drv messages are synchronous, so
 * this sleeps.
 */
static int ppe_nss_port_start(struct ppe_nss_port *port)
{
	struct net_device *netdev = port->netdev;
	int vsi, ret;

	ASSERT_RTNL();

	/*
	 * The firmware delivers ingress only for ports with an assigned
	 * VSI (measured: open without vsi_assign = TX flows, RX dead),
	 * so a port without one must stay down rather than half-start.
	 * The VSI must also be private to this port: the firmware NACKs
	 * assigning a VSI that another physical port already holds
	 * (measured), so qca_ppe hands out one per port and the Linux
	 * bridge stays the forwarding authority for bridged ports.
	 */
	vsi = qca_ppe_port_fw_vsi_get(netdev);
	if (vsi < 0 && port->dp_ops->vsi_assign) {
		netdev_warn(netdev, "qca-ppe-nss: no PPE VSI (%d), not starting\n",
			    vsi);
		return -ENOENT;
	}
	if (port->dp_ops->vsi_assign) {
		if (port->dp_ops->vsi_assign(port->dpc, vsi)) {
			netdev_warn(netdev, "qca-ppe-nss: fw vsi_assign(%d) failed\n",
				    vsi);
			return -EIO;
		}
		port->fw_vsi = vsi;
		/*
		 * The fw zeroes the VSI's member/flood masks on assign,
		 * which would silence its own broadcast egress; have
		 * qca_ppe re-assert them. Unlike the missing-VSI case
		 * above this cannot refuse the start: it only fails when
		 * the port has no VSI or is not a qca-ppe port, both of
		 * which the get() above just disproved under this same
		 * rtnl, so the warning marks an impossibility rather than
		 * a condition worth stopping a working port for.
		 */
		if (qca_ppe_port_fw_vsi_refresh(netdev))
			netdev_warn(netdev, "qca-ppe-nss: fw vsi refresh failed\n");
	}

	if (port->dp_ops->mac_addr(port->dpc, (uint8_t *)netdev->dev_addr)) {
		netdev_warn(netdev, "qca-ppe-nss: fw mac_addr failed\n");
		goto err_vsi;
	}

	if (port->dp_ops->change_mtu(port->dpc, netdev->mtu)) {
		netdev_warn(netdev, "qca-ppe-nss: fw change_mtu(%d) failed\n",
			    netdev->mtu);
		goto err_vsi;
	}

	if (port->dp_ops->open(port->dpc, 0, 0, 0)) {
		netdev_warn(netdev, "qca-ppe-nss: fw open failed\n");
		goto err_vsi;
	}

	/*
	 * A freshly opened firmware interface starts with its link down, so the
	 * shadow has to say so - otherwise the grant that follows would be
	 * skipped as redundant and the port would never be told it is up.
	 */
	port->fw_link_up = false;
	port->fw_link_failed = 0;

	ret = qca_edma_port_dp_claim(port->conduit, port->if_num,
				     &ppe_nss_dp_owner, port);
	if (ret) {
		netdev_warn(netdev, "qca-ppe-nss: datapath claim failed (%d)\n",
			    ret);
		port->dp_ops->close(port->dpc);
		goto err_vsi;
	}

	port->state = PPE_NSS_PORT_STARTED;
	netdev_info(netdev, "qca-ppe-nss: port %d on NSS fw data plane (vsi %d)\n",
		    port->if_num, port->fw_vsi);
	return 0;

err_vsi:
	if (port->fw_vsi >= 0 && port->dp_ops->vsi_unassign) {
		port->dp_ops->vsi_unassign(port->dpc, port->fw_vsi);
		port->fw_vsi = -1;
	}
	return -EIO;
}

/*
 * Unwind a port down the state ladder to @target. With @fw_alive the
 * firmware is messaged (link down, vsi unassign, close); without, only
 * host-side state is undone - the rmmod path must not message the fw,
 * see nss_dp_restore_data_plane(). Caller holds rtnl and ppe_nss_lock.
 */
static void ppe_nss_port_unwind(struct ppe_nss_port *port,
				enum ppe_nss_port_state target, bool fw_alive)
{
	if (port->state == PPE_NSS_PORT_STARTED &&
	    target < PPE_NSS_PORT_STARTED) {
		/* Returns with no fw xmit in flight; host TX path is back */
		qca_edma_port_dp_release(port->conduit, port->if_num);

		if (fw_alive) {
			ppe_nss_fw_link_state(port, false);

			if (port->fw_vsi >= 0 && port->dp_ops->vsi_unassign &&
			    port->dp_ops->vsi_unassign(port->dpc, port->fw_vsi))
				netdev_warn(port->netdev, "qca-ppe-nss: fw vsi_unassign failed\n");

			if (port->dp_ops->close(port->dpc))
				netdev_warn(port->netdev, "qca-ppe-nss: fw close failed\n");

			netdev_info(port->netdev, "qca-ppe-nss: port %d back on host data plane\n",
				    port->if_num);
		}

		port->fw_vsi = -1;
		port->state = PPE_NSS_PORT_OVERRIDDEN;
	}

	if (port->state == PPE_NSS_PORT_OVERRIDDEN &&
	    target < PPE_NSS_PORT_OVERRIDDEN) {
		if (port->headroom_added) {
			port->netdev->needed_headroom -= NSS_DP_PREHEADER_SIZE;
			port->headroom_added = false;
		}
		port->dp_ops = NULL;
		port->dpc = NULL;
		port->state = PPE_NSS_PORT_ARMED;
		netdev_info(port->netdev, "qca-ppe-nss: NSS data plane released on port %d\n",
			    port->if_num);
	}

	if (port->state == PPE_NSS_PORT_ARMED && target < PPE_NSS_PORT_ARMED) {
		ppe_nss_port_put(port);
		port->state = PPE_NSS_PORT_IDLE;
	}
}

struct net_device *nss_dp_get_netdev_by_nss_if_num(int if_num)
{
	struct net_device *netdev = NULL;

	if (if_num < NSS_DP_START_IFNUM || if_num >= NSS_DP_MAX_INTERFACES)
		return NULL;

	mutex_lock(&ppe_nss_lock);
	if (ppe_nss_ports[if_num].state >= PPE_NSS_PORT_ARMED)
		netdev = ppe_nss_ports[if_num].netdev;
	mutex_unlock(&ppe_nss_lock);

	return netdev;
}
EXPORT_SYMBOL(nss_dp_get_netdev_by_nss_if_num);

bool nss_dp_is_in_open_state(struct net_device *netdev)
{
	return netif_running(netdev);
}
EXPORT_SYMBOL(nss_dp_is_in_open_state);

int nss_dp_override_data_plane(struct net_device *netdev,
			       struct nss_dp_data_plane_ops *dp_ops,
			       struct nss_dp_data_plane_ctx *dpc)
{
	int ret = NSS_DP_FAILURE;
	int i;

	if (!netdev || !dp_ops || !dpc || !dp_ops->init || !dp_ops->open ||
	    !dp_ops->close || !dp_ops->link_state || !dp_ops->mac_addr ||
	    !dp_ops->change_mtu || !dp_ops->xmit)
		return NSS_DP_FAILURE;

	mutex_lock(&ppe_nss_lock);
	for (i = NSS_DP_START_IFNUM; i < NSS_DP_MAX_INTERFACES; i++) {
		struct ppe_nss_port *port = &ppe_nss_ports[i];

		if (port->state < PPE_NSS_PORT_ARMED || port->netdev != netdev)
			continue;

		if (port->state >= PPE_NSS_PORT_OVERRIDDEN) {
			/* idempotent for nss-drv's per-core loops */
			ret = (port->dpc == dpc) ? NSS_DP_SUCCESS
						 : NSS_DP_FAILURE;
			break;
		}

		port->dp_ops = dp_ops;
		port->dpc = dpc;

		/* adds the 32B preheader to the port's needed_headroom */
		if (dp_ops->init(dpc)) {
			netdev_warn(netdev, "qca-ppe-nss: dp init failed\n");
			port->dp_ops = NULL;
			port->dpc = NULL;
			break;
		}
		port->headroom_added = true;
		port->state = PPE_NSS_PORT_OVERRIDDEN;

		netdev_info(netdev, "qca-ppe-nss: NSS data plane override on port %d\n",
			    i);
		ret = NSS_DP_SUCCESS;
		break;
	}
	mutex_unlock(&ppe_nss_lock);

	return ret;
}
EXPORT_SYMBOL(nss_dp_override_data_plane);

/*
 * nss-drv signals "registration done, port was open" here (workqueue,
 * process context). For a port that is down, the NETDEV_UP notifier
 * does this instead.
 */
void nss_dp_start_data_plane(struct net_device *netdev,
			     struct nss_dp_data_plane_ctx *dpc)
{
	int i;

	rtnl_lock();
	mutex_lock(&ppe_nss_lock);
	for (i = NSS_DP_START_IFNUM; i < NSS_DP_MAX_INTERFACES; i++) {
		struct ppe_nss_port *port = &ppe_nss_ports[i];

		if (port->state < PPE_NSS_PORT_OVERRIDDEN ||
		    port->netdev != netdev)
			continue;
		if (port->dpc != dpc) {
			netdev_warn(netdev, "qca-ppe-nss: start with foreign dpc, ignored\n");
			break;
		}
		if (port->state == PPE_NSS_PORT_OVERRIDDEN)
			ppe_nss_port_start(port);
		break;
	}
	mutex_unlock(&ppe_nss_lock);
	rtnl_unlock();
}
EXPORT_SYMBOL(nss_dp_start_data_plane);

/*
 * nss-drv detaches at rmmod. Its IRQs are already torn down by the
 * time this runs (nss_hal_remove), so firmware messages could only
 * time out: do host-side cleanup only. The next modprobe boots the
 * firmware from scratch and re-registers.
 */
void nss_dp_restore_data_plane(struct net_device *netdev)
{
	struct net_device *conduit = NULL;
	bool overridden_left = false;
	int i;

	rtnl_lock();
	mutex_lock(&ppe_nss_lock);
	for (i = NSS_DP_START_IFNUM; i < NSS_DP_MAX_INTERFACES; i++) {
		struct ppe_nss_port *port = &ppe_nss_ports[i];

		if (port->state < PPE_NSS_PORT_OVERRIDDEN ||
		    port->netdev != netdev)
			continue;

		conduit = port->conduit;
		ppe_nss_port_unwind(port, PPE_NSS_PORT_ARMED, false);
	}

	for (i = NSS_DP_START_IFNUM; i < NSS_DP_MAX_INTERFACES; i++)
		if (ppe_nss_ports[i].state >= PPE_NSS_PORT_OVERRIDDEN)
			overridden_left = true;

	/*
	 * Last override released: nss-drv is detaching and nss_hal_remove
	 * has already put the core in reset. Give the host its EDMA back -
	 * the firmware's QID2RID takeover otherwise leaves every CPU-port
	 * queue pointing at dead firmware rings, killing wired RX until
	 * reboot, and a later firmware boot would come up over the live
	 * ring state of its predecessor and crash in early init.
	 */
	if (conduit && !overridden_left &&
	    qca_edma_fw_baseline_restore(conduit) == 0)
		pr_info("qca-ppe-nss: host EDMA baseline restored\n");

	mutex_unlock(&ppe_nss_lock);
	rtnl_unlock();
}
EXPORT_SYMBOL(nss_dp_restore_data_plane);

/*
 * N2H receive: every packet/exception the firmware sends for a
 * registered phys_if lands here (nss-drv NAPI). Deliver on the user
 * port, mirroring the DSA OOB host receive path (the conduit
 * legitimately never saw the frame; offload_fwd_mark stays 0 exactly
 * as with the OOB tagger).
 */
void nss_dp_receive(struct net_device *netdev, struct sk_buff *skb,
		    struct napi_struct *napi)
{
	struct ppe_nss_port *port = NULL;
	int i;

	if (likely(netdev)) {
		for (i = NSS_DP_START_IFNUM; i < NSS_DP_MAX_INTERFACES; i++) {
			if (ppe_nss_ports[i].netdev == netdev) {
				port = &ppe_nss_ports[i];
				break;
			}
		}
	}

	if (unlikely(!port)) {
		atomic_inc(&ppe_nss_rx_unexpected);
		dev_kfree_skb_any(skb);
		return;
	}

	atomic64_inc(&port->rx_fw_pkts);

	skb->dev = netdev;
	skb->protocol = eth_type_trans(skb, netdev);
	dev_sw_netstats_rx_add(netdev, skb->len + ETH_HLEN);
	napi_gro_receive(napi, skb);
}
EXPORT_SYMBOL(nss_dp_receive);

/*
 * PTP timestamp RX delivery, registered by nss-drv as service code 0x9.
 * Same semantics as the old nss-dp implementation: give the PHY driver
 * a chance to timestamp, otherwise deliver to the stack.
 */
void nss_phy_tstamp_rx_buf(void *app_data, struct sk_buff *skb)
{
	struct net_device *ndev = skb->dev;

	if (ndev && phy_has_rxtstamp(ndev->phydev))
		if (phy_rxtstamp(ndev->phydev, skb, 0))
			return;

	netif_receive_skb(skb);
}
EXPORT_SYMBOL(nss_phy_tstamp_rx_buf);

/*
 * ===== nss-drv deferred probe =====
 *
 * qca-nss-drv defers its platform probe - and with it the NSS firmware
 * boot - until at least one port is armed here. This keeps the module
 * dependency chain from Wi-Fi safe: with ath11k NSS offload compiled in,
 * ath11k.ko and mac80211.ko carry hard symbol references to qca-nss-drv,
 * so kmodloader pulls it in at every boot. Booting the firmware with no
 * armed ports kills all wired RX (the fw QID2RID takeover unmaps the
 * host EDMA ring from every queue), so the probe must wait for arming.
 * Deferred core devices are recorded and re-attached when fw_mask first
 * becomes non-zero.
 */

/*
 * One slot per NSS core: qca-nss-drv binds a platform device per core and
 * indexes its own per-core array with that device's id, so it can never
 * carry more than NSS_MAX_CORES of them (2 - nss-drv exports/nss_api_if.h,
 * unconditional across every SoC it supports; ipq8074-nss.dtsi instantiates
 * exactly nss0 and nss1). This is a property of that driver's API rather
 * than of the board, so there is nothing to derive from the live topology -
 * but a device that finds no slot stays deferred with nothing left to
 * re-attach it, so it is reported rather than dropped.
 */
#define PPE_NSS_GATE_MAX_DEVS 2
static DEFINE_SPINLOCK(ppe_nss_gate_lock);
static struct device *ppe_nss_gate_devs[PPE_NSS_GATE_MAX_DEVS];

int nss_dp_probe_gate(struct device *dev)
{
	bool recorded = false;
	int i;

	spin_lock(&ppe_nss_gate_lock);
	if (READ_ONCE(ppe_nss_fw_mask)) {
		spin_unlock(&ppe_nss_gate_lock);
		return 0;
	}
	for (i = 0; i < PPE_NSS_GATE_MAX_DEVS; i++) {
		if (ppe_nss_gate_devs[i] == dev)
			break;
		if (!ppe_nss_gate_devs[i]) {
			ppe_nss_gate_devs[i] = get_device(dev);
			recorded = true;
			break;
		}
	}
	spin_unlock(&ppe_nss_gate_lock);

	/* the driver core retries deferred probes often - log once */
	if (recorded)
		dev_info(dev, "qca-ppe-nss: deferring NSS core probe until a port is armed\n");
	else if (i == PPE_NSS_GATE_MAX_DEVS)
		dev_warn_once(dev, "qca-ppe-nss: more NSS core devices than cores, this one will stay deferred\n");
	return -EPROBE_DEFER;
}
EXPORT_SYMBOL(nss_dp_probe_gate);

static void ppe_nss_gate_kick(void)
{
	struct device *devs[PPE_NSS_GATE_MAX_DEVS];
	int i;

	spin_lock(&ppe_nss_gate_lock);
	memcpy(devs, ppe_nss_gate_devs, sizeof(devs));
	memset(ppe_nss_gate_devs, 0, sizeof(ppe_nss_gate_devs));
	spin_unlock(&ppe_nss_gate_lock);

	for (i = 0; i < PPE_NSS_GATE_MAX_DEVS; i++) {
		if (!devs[i])
			continue;
		if (device_attach(devs[i]) < 0)
			dev_warn(devs[i],
				 "qca-ppe-nss: deferred NSS core attach failed\n");
		put_device(devs[i]);
	}
}

static void ppe_nss_gate_drop(void)
{
	int i;

	spin_lock(&ppe_nss_gate_lock);
	for (i = 0; i < PPE_NSS_GATE_MAX_DEVS; i++) {
		if (ppe_nss_gate_devs[i]) {
			put_device(ppe_nss_gate_devs[i]);
			ppe_nss_gate_devs[i] = NULL;
		}
	}
	spin_unlock(&ppe_nss_gate_lock);
}

/*
 * ===== fw_mask arming (debugfs) =====
 */

static int ppe_nss_fw_arm(int if_num)
{
	struct ppe_nss_port *port = &ppe_nss_ports[if_num];
	int ret;

	if (port->state != PPE_NSS_PORT_IDLE)
		return 0;

	ret = ppe_nss_port_hold(port, if_num);
	if (ret)
		return ret;

	port->state = PPE_NSS_PORT_ARMED;
	netdev_info(port->netdev, "qca-ppe-nss: port %d armed for NSS fw attach\n",
		    if_num);
	return 0;
}

static int ppe_nss_fw_disarm(int if_num)
{
	struct ppe_nss_port *port = &ppe_nss_ports[if_num];

	if (port->state == PPE_NSS_PORT_IDLE)
		return 0;
	if (port->state >= PPE_NSS_PORT_OVERRIDDEN) {
		/* registration is once per fw boot; mask change = drv reload */
		netdev_warn(port->netdev,
			    "qca-ppe-nss: port %d attached to nss-drv, reload qca-nss-drv to disarm\n",
			    if_num);
		return -EBUSY;
	}

	netdev_info(port->netdev, "qca-ppe-nss: port %d disarmed\n", if_num);
	ppe_nss_port_unwind(port, PPE_NSS_PORT_IDLE, false);
	return 0;
}

/*
 * Every if_num that currently has a DSA user port on a qca_edma conduit -
 * i.e. every wired port this board can offload. The set is discovered from
 * the live DSA topology, not hardcoded, so the same logic works across the
 * whole ipq807x family (the per-board port<->if_num map differs: AX3600 uses
 * if_num 2-5, the DL-WRX36 uses 1-4 + 6). Caller holds rtnl.
 */
static unsigned long ppe_nss_discover_mask(void)
{
	struct net_device *conduit;
	unsigned long mask = 0;
	int i;

	ASSERT_RTNL();
	for (i = NSS_DP_START_IFNUM; i < NSS_DP_MAX_INTERFACES; i++)
		if (ppe_nss_find_user_port(i, &conduit))
			__set_bit(i, &mask);
	return mask;
}

static int ppe_nss_fw_mask_apply(unsigned long mask, bool discover)
{
	int ret = 0;
	int i;

	rtnl_lock();
	mutex_lock(&ppe_nss_lock);

	if (discover)
		mask = ppe_nss_discover_mask();

	if (mask & ~GENMASK(NSS_DP_MAX_INTERFACES - 1, NSS_DP_START_IFNUM)) {
		ret = -EINVAL;
		goto out;
	}

	for (i = NSS_DP_START_IFNUM; i < NSS_DP_MAX_INTERFACES; i++) {
		int err = 0;

		if (test_bit(i, &mask) && !test_bit(i, &ppe_nss_fw_mask))
			err = ppe_nss_fw_arm(i);
		else if (!test_bit(i, &mask) && test_bit(i, &ppe_nss_fw_mask))
			err = ppe_nss_fw_disarm(i);
		/*
		 * One port failing says nothing about the others, and
		 * abandoning the walk here left the requested set half
		 * applied with nothing to finish it. Apply every port on
		 * its own - both paths name the port they could not do
		 * and why - and report the first error. fw_mask keeps
		 * naming exactly the ports that ended up armed either way.
		 */
		if (err && !ret)
			ret = err;
		if (ppe_nss_ports[i].state >= PPE_NSS_PORT_ARMED)
			__set_bit(i, &ppe_nss_fw_mask);
		else
			__clear_bit(i, &ppe_nss_fw_mask);
	}
out:
	mutex_unlock(&ppe_nss_lock);
	rtnl_unlock();

	return ret;
}

static ssize_t ppe_nss_fw_mask_write(struct file *file,
				     const char __user *ubuf,
				     size_t len, loff_t *ppos)
{
	unsigned long mask = 0;
	bool discover = false;
	char buf[24];
	int ret;

	if (len == 0 || len >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = '\0';

	/*
	 * "all"/"auto" arms every wired port this board exposes (device
	 * agnostic); an explicit hex mask still selects a subset. */
	if (sysfs_streq(buf, "all") || sysfs_streq(buf, "auto")) {
		discover = true;
	} else {
		ret = kstrtoul(strim(buf), 0, &mask);
		if (ret)
			return ret;
	}

	ret = ppe_nss_fw_mask_apply(mask, discover);

	/*
	 * Release the probe-gated NSS core devices for whatever did arm.
	 * A partially applied mask still leaves ports waiting on the
	 * firmware data plane, and returning the error without booting it
	 * would leave fw_mask naming armed ports that nothing will ever
	 * attach to - and the mask is all the bring-up can see.
	 */
	if (ppe_nss_fw_mask)
		ppe_nss_gate_kick();

	return ret ? ret : len;
}

static int ppe_nss_fw_mask_show(struct seq_file *m, void *v)
{
	seq_printf(m, "0x%lx\n", ppe_nss_fw_mask);
	return 0;
}

static int ppe_nss_fw_mask_open(struct inode *inode, struct file *file)
{
	return single_open(file, ppe_nss_fw_mask_show, NULL);
}

static const struct file_operations ppe_nss_fw_mask_fops = {
	.owner = THIS_MODULE,
	.open = ppe_nss_fw_mask_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
	.write = ppe_nss_fw_mask_write,
};

/*
 * ===== netdev notifier =====
 *
 * Runs under rtnl; takes only ppe_nss_lock (lock order rtnl ->
 * ppe_nss_lock). Tracks the lifecycle of fw-attached ports and tears
 * everything down before a tracked netdev goes away.
 */
static void ppe_nss_event_unregister(struct net_device *dev)
{
	int i;

	for (i = NSS_DP_START_IFNUM; i < NSS_DP_MAX_INTERFACES; i++) {
		struct ppe_nss_port *port = &ppe_nss_ports[i];

		if (port->netdev != dev && port->conduit != dev)
			continue;

		if (port->state == PPE_NSS_PORT_IDLE)
			continue;

		/*
		 * nss-drv keeps stale references to this netdev until it
		 * is reloaded; RX for this phys_if is dropped from now on
		 * (lookup by netdev fails once the slot is unwound).
		 */
		if (port->state >= PPE_NSS_PORT_OVERRIDDEN)
			netdev_warn(dev,
				    "qca-ppe-nss: unregister while attached to nss-drv, reload qca-nss-drv\n");

		ppe_nss_port_unwind(port, PPE_NSS_PORT_IDLE, true);
		__clear_bit(i, &ppe_nss_fw_mask);
	}
}

static int ppe_nss_netdev_event(struct notifier_block *nb,
				unsigned long event, void *ptr)
{
	struct net_device *dev = netdev_notifier_info_to_dev(ptr);
	struct ppe_nss_port *port = NULL;
	int i;

	mutex_lock(&ppe_nss_lock);

	if (event == NETDEV_UNREGISTER) {
		ppe_nss_event_unregister(dev);
		goto out;
	}

	for (i = NSS_DP_START_IFNUM; i < NSS_DP_MAX_INTERFACES; i++) {
		if (ppe_nss_ports[i].state >= PPE_NSS_PORT_ARMED &&
		    ppe_nss_ports[i].netdev == dev) {
			port = &ppe_nss_ports[i];
			break;
		}
	}
	if (!port)
		goto out;

	switch (event) {
	case NETDEV_UP:
		if (port->state == PPE_NSS_PORT_OVERRIDDEN)
			ppe_nss_port_start(port);
		break;
	case NETDEV_DOWN:
		ppe_nss_port_unwind(port, PPE_NSS_PORT_OVERRIDDEN, true);
		break;
	case NETDEV_CHANGEMTU:
		if (port->state == PPE_NSS_PORT_STARTED &&
		    port->dp_ops->change_mtu(port->dpc, dev->mtu))
			netdev_warn(dev, "qca-ppe-nss: fw change_mtu(%d) failed\n",
				    dev->mtu);
		break;
	case NETDEV_CHANGEADDR:
		if (port->state == PPE_NSS_PORT_STARTED &&
		    port->dp_ops->mac_addr(port->dpc, (uint8_t *)dev->dev_addr))
			netdev_warn(dev, "qca-ppe-nss: fw mac_addr failed\n");
		break;
	}

out:
	mutex_unlock(&ppe_nss_lock);
	return NOTIFY_DONE;
}

static struct notifier_block ppe_nss_netdev_nb = {
	.notifier_call = ppe_nss_netdev_event,
};

static int ppe_nss_status_show(struct seq_file *m, void *v)
{
	int i;

	seq_printf(m, "rx_unexpected: %d\n",
		   atomic_read(&ppe_nss_rx_unexpected));
	seq_printf(m, "fw_mask: 0x%lx\n", ppe_nss_fw_mask);
	for (i = NSS_DP_START_IFNUM; i < NSS_DP_MAX_INTERFACES; i++) {
		struct ppe_nss_port *port = &ppe_nss_ports[i];

		/*
		 * tx_redirect_pkts counts what we handed to nss-drv, not what
		 * the firmware took: nss-drv returns NETDEV_TX_OK for every
		 * failure except a full queue, so a port whose frames are all
		 * being refused still reads as if it were transmitting. It
		 * counts those refusals in the netdev's tx_dropped, so report
		 * that next to it - the difference is what actually reached the
		 * firmware, and without it a wedged port is unreadable here.
		 */
		seq_printf(m,
			   "if_num %d: netdev=%s armed=%d overridden=%d started=%d injectable=%d fw_link=%d fw_link_failed=%d fw_link_changes=%d fw_link_skipped=%d fw_vsi=%d tx_redirect_pkts=%lld tx_dropped=%lu tx_ungranted=%llu tx_busy=%lld rx_fw_pkts=%lld\n",
			   i,
			   port->netdev ? netdev_name(port->netdev) : "-",
			   port->state >= PPE_NSS_PORT_ARMED,
			   port->state >= PPE_NSS_PORT_OVERRIDDEN,
			   port->state == PPE_NSS_PORT_STARTED,
			   port->state == PPE_NSS_PORT_STARTED &&
			   qca_edma_port_dp_injectable(port->conduit,
						      port->if_num),
			   port->fw_link_up,
			   port->fw_link_failed,
			   atomic_read(&port->fw_link_changes),
			   atomic_read(&port->fw_link_skipped),
			   port->fw_vsi,
			   (long long)atomic64_read(&port->tx_redirect_pkts),
			   port->netdev ? port->netdev->stats.tx_dropped : 0UL,
			   qca_edma_port_dp_tx_ungranted(port->conduit,
							 port->if_num),
			   (long long)atomic64_read(&port->tx_busy),
			   (long long)atomic64_read(&port->rx_fw_pkts));
	}
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(ppe_nss_status);

/*
 * Clocks the NSS block requires while the firmware runs and that no host
 * driver on the qca_edma/qca_ppe stack enables. In the old stack these were enabled by
 * qca-ssdk / qca-nss-crypto. The firmware accesses these blocks (NSS core 1
 * boots with crypto/ipsec/tls features) and a gated clock turns that access
 * into a silent NoC stall - observed as a hard SoC hang right after "NSS
 * core 1 booted successfully".
 *
 * Which clocks those are differs per SoC, so the list is the one the
 * nss-common node carries rather than a table here.
 */
static void ppe_nss_enable_aux_clks(void)
{
	struct device_node *np;
	int count, i;

	np = of_find_compatible_node(NULL, NULL, "qcom,nss-common");
	if (!np) {
		pr_warn("qca-ppe-nss: nss-common node not found, aux clocks not enabled\n");
		return;
	}

	count = of_property_count_strings(np, "clock-names");
	for (i = 0; i < count; i++) {
		const char *name = NULL;
		struct clk *clk;

		of_property_read_string_index(np, "clock-names", i, &name);

		clk = of_clk_get(np, i);
		if (IS_ERR(clk)) {
			pr_warn("qca-ppe-nss: %s lookup failed (%ld)\n",
				name, PTR_ERR(clk));
			continue;
		}

		/*
		 * Held for the lifetime of the firmware, so never put back.
		 */
		if (clk_prepare_enable(clk)) {
			pr_warn("qca-ppe-nss: %s enable failed\n", name);
			clk_put(clk);
			continue;
		}

		pr_info("qca-ppe-nss: enabled %s for NSS fw\n", name);
	}

	of_node_put(np);
}

static int __init qca_ppe_nss_init(void)
{
	int ret;
	int i;

	for (i = 0; i < NSS_DP_MAX_INTERFACES; i++)
		ppe_nss_ports[i].fw_vsi = -1;

	ret = register_netdevice_notifier(&ppe_nss_netdev_nb);
	if (ret)
		return ret;

	ppe_nss_enable_aux_clks();
	ppe_nss_dentry = debugfs_create_dir("qca-ppe-nss", NULL);
	debugfs_create_file("status", 0444, ppe_nss_dentry, NULL,
			    &ppe_nss_status_fops);
	debugfs_create_file("fw_mask", 0644, ppe_nss_dentry, NULL,
			    &ppe_nss_fw_mask_fops);
	pr_info("qca-ppe-nss: NSS data-plane glue loaded (phase 2b: fw data plane)\n");
	return 0;
}

static void __exit qca_ppe_nss_exit(void)
{
	int i;

	/* No new attaches once the debugfs files are gone */
	debugfs_remove_recursive(ppe_nss_dentry);

	/*
	 * qca-nss-drv pins this module through the nss_dp_* symbols, so
	 * no port can be overridden or started here; only armed-but-idle
	 * ports remain.
	 */
	mutex_lock(&ppe_nss_lock);
	for (i = NSS_DP_START_IFNUM; i < NSS_DP_MAX_INTERFACES; i++)
		ppe_nss_port_unwind(&ppe_nss_ports[i], PPE_NSS_PORT_IDLE,
				    false);
	mutex_unlock(&ppe_nss_lock);

	ppe_nss_gate_drop();
	unregister_netdevice_notifier(&ppe_nss_netdev_nb);
}

module_init(qca_ppe_nss_init);
module_exit(qca_ppe_nss_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("NSS data-plane glue for the qca_edma/qca_ppe stack");
