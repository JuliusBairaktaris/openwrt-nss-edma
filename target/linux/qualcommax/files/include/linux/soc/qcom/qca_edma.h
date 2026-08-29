/* SPDX-License-Identifier: GPL-2.0-only */
/* What the qca-edma conduit driver lends the NSS firmware data plane. */

#ifndef __LINUX_SOC_QCOM_QCA_EDMA_H__
#define __LINUX_SOC_QCOM_QCA_EDMA_H__

#include <linux/netdevice.h>

#if IS_REACHABLE(CONFIG_QCOM_EDMA)
int qca_edma_cpu_queues_to_host(struct net_device *conduit);
#else
static inline int qca_edma_cpu_queues_to_host(struct net_device *conduit)
{
	return -ENODEV;
}
#endif

#endif /* __LINUX_SOC_QCOM_QCA_EDMA_H__ */
