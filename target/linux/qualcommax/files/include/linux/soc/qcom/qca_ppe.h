/* SPDX-License-Identifier: GPL-2.0-only */
/* What the qca-ppe DSA driver lends the NSS firmware data plane. qca-ppe
 * stays the only writer of the PPE tables.
 */

#ifndef __LINUX_SOC_QCOM_QCA_PPE_H__
#define __LINUX_SOC_QCOM_QCA_PPE_H__

#include <linux/netdevice.h>

int qca_ppe_port_fw_vsi_get(struct net_device *netdev);
int qca_ppe_port_vsi_restore(struct net_device *netdev);

#endif /* __LINUX_SOC_QCOM_QCA_PPE_H__ */
