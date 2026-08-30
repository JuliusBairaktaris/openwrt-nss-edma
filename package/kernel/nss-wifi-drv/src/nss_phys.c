// SPDX-License-Identifier: GPL-2.0-only
/* Handing a switch port to the firmware to route through.
 *
 * A running core already delivers everything arriving at the switch's CPU
 * port, and until a port is opened the firmware forwards that straight to the
 * host. That is what makes the exception path work, and it is also why no
 * connection rule can ever match: the packet never reaches the engine the rule
 * lives in. Opening a port points its receive at the firmware's own ethernet
 * node instead, which is where the IPv4 and IPv6 engines are.
 *
 * Three things have to be said before the port is opened, because none of them
 * comes from the rule: the address, which appears as the source address of
 * everything transmitted there; the maximum frame size; and the switch's own
 * forwarding identity for the port, without which the port has ingress and
 * nothing leaves through the fabric. The link state comes last, because a
 * freshly opened interface starts with its link down and nothing transmits on
 * it until the link is up.
 *
 * The port set is whatever the switch topology has: the driver already holds
 * one network device per user port, so a port is named by its index and the
 * caller asks for a mask. Arming is a deliberate runtime act, like starting
 * the core, and the host's own transmit path is untouched by it - the firmware
 * gains a way to transmit on the port, it does not take the host's.
 */

#include <linux/bitops.h>
#include <linux/cleanup.h>
#include <linux/etherdevice.h>
#include <linux/netdevice.h>
#include <linux/rtnetlink.h>
#include <linux/soc/qcom/qca_ppe.h>
#include <net/dsa.h>

#include "nss_drv.h"
/* The interface header names a shaper configuration in a message this driver
 * does not send, and does not carry the definition itself.
 */
#include "nss_shaper.h"
#include "nss_if.h"

/* Where the firmware sends what a port receives once the port is open: its
 * ethernet node, which is what feeds the connection engines.
 */
#define NSS_PHYS_RX_FORWARD	NSS_INTERFACE_ETH_RX

static int nss_phys_msg(struct nss_core *core, int if_num, u32 type,
			const void *body, size_t len)
{
	struct nss_if_msg *m __free(kfree) = kzalloc(sizeof(*m), GFP_KERNEL);
	int ret;

	if (!m)
		return -ENOMEM;

	m->cm.interface = if_num;
	m->cm.type = type;
	if (body)
		memcpy(&m->msg, body, len);

	ret = nss_msg_send(core, m, offsetof(struct nss_if_msg, msg) + len);
	if (!ret && m->cm.error)
		ret = -EIO;

	return ret;
}

static int nss_phys_port_open(struct nss_core *core, int if_num,
			      struct net_device *dev)
{
	struct nss_if_link_state_notify link = { .state = 1 };
	struct nss_if_mac_address_set mac = {};
	struct nss_if_mtu_change mtu = {};
	struct nss_if_vsi_assign vsi = {};
	struct nss_if_open open = {};
	int ret, id;

	/* The identity's member and flood masks read back zero after the
	 * assign, which would silence the port's own broadcast egress, so the
	 * switch driver reasserts them straight after.
	 */
	id = qca_ppe_port_fw_vsi_get(dev);
	if (id < 0)
		return id;

	vsi.vsi = id;
	ret = nss_phys_msg(core, if_num, NSS_IF_VSI_ASSIGN, &vsi, sizeof(vsi));
	if (ret)
		return ret;

	qca_ppe_port_fw_vsi_refresh(dev);

	ether_addr_copy(mac.mac_addr, dev->dev_addr);
	ret = nss_phys_msg(core, if_num, NSS_IF_MAC_ADDR_SET, &mac, sizeof(mac));
	if (ret)
		return ret;

	mtu.min_buf_size = dev->mtu;
	ret = nss_phys_msg(core, if_num, NSS_IF_MTU_CHANGE, &mtu, sizeof(mtu));
	if (ret)
		return ret;

	open.rx_forward_if = NSS_PHYS_RX_FORWARD;
	open.alignment_mode = NSS_IF_DATA_ALIGN_2BYTE;
	ret = nss_phys_msg(core, if_num, NSS_IF_OPEN, &open, sizeof(open));
	if (ret)
		return ret;

	ret = nss_phys_msg(core, if_num, NSS_IF_LINK_STATE_NOTIFY, &link,
			   sizeof(link));
	if (ret)
		return ret;

	dev_info(core->dev, "%s: port %d routes in the firmware, vsi %d\n",
		 netdev_name(dev), if_num, id);

	return 0;
}

static void nss_phys_port_close(struct nss_core *core, int if_num,
				struct net_device *dev)
{
	struct nss_if_link_state_notify link = {};
	struct nss_if_vsi_assign vsi = {};

	nss_phys_msg(core, if_num, NSS_IF_LINK_STATE_NOTIFY, &link,
		     sizeof(link));

	vsi.vsi = qca_ppe_port_fw_vsi_get(dev);
	if (vsi.vsi >= 0)
		nss_phys_msg(core, if_num, NSS_IF_VSI_UNASSIGN, &vsi,
			     sizeof(vsi));

	nss_phys_msg(core, if_num, NSS_IF_CLOSE, NULL, 0);

	/* The port's binding reads back cleared after the unassign, so the
	 * switch driver puts back the one the bridge implies.
	 */
	qca_ppe_port_vsi_restore(dev);

	dev_info(core->dev, "%s: port %d back to the host\n",
		 netdev_name(dev), if_num);
}

/* Bring the set of firmware-routed ports to exactly @mask.
 *
 * A port already in the state the mask asks for is left alone rather than
 * reopened: the firmware's view of a port changes only when it is changed,
 * and restating it costs a round trip and gains nothing.
 */
int nss_phys_arm(struct nss_core *core, unsigned long mask)
{
	unsigned long armed = core->phys_armed;
	unsigned long changed;
	int ret = 0;
	unsigned int i;

	if (!core->running)
		return -ENODEV;

	/* The switch driver reads and writes a port's forwarding identity
	 * under rtnl, and this is what asks it to.
	 */
	ASSERT_RTNL();

	/* Only what the mask changes, and only as far as a mask reaches: a
	 * port index beyond the width of one is not addressable and shifting
	 * by it does not mean what it looks like.
	 */
	changed = mask ^ armed;
	for_each_set_bit(i, &changed, BITS_PER_LONG) {
		bool want = mask & BIT(i);
		struct net_device *dev;

		dev = rcu_dereference_protected(core->iface[i],
						lockdep_is_held(&core->lock));
		if (!dev || !dsa_user_dev_check(dev)) {
			if (want)
				ret = -ENODEV;
			continue;
		}

		if (want) {
			int err = nss_phys_port_open(core, i, dev);

			if (err) {
				dev_warn(core->dev, "%s: port %d not armed: %d\n",
					 netdev_name(dev), i, err);
				ret = err;
				continue;
			}
			core->phys_armed |= BIT(i);
		} else {
			nss_phys_port_close(core, i, dev);
			core->phys_armed &= ~BIT(i);
		}
	}

	return ret;
}
