/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * PPE state exported by the qca-ppe DSA driver to the NSS data-plane
 * glue.
 *
 * qca-ppe is the single writer of all PPE tables; other modules
 * observe its decisions (or request firmware-side resources) through
 * these calls instead of touching the registers themselves.
 */

#ifndef __LINUX_SOC_QCOM_QCA_PPE_H__
#define __LINUX_SOC_QCOM_QCA_PPE_H__

#include <linux/netdevice.h>

int qca_ppe_port_fw_vsi_get(struct net_device *netdev);
int qca_ppe_port_fw_vsi_refresh(struct net_device *netdev);

#endif /* __LINUX_SOC_QCOM_QCA_PPE_H__ */
