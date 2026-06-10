/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Per-port TX redirect API exported by the qca-edma conduit driver.
 *
 * A redirect claims all conduit TX for one PPE port (resolved from the
 * DSA out-of-band tag) before the frame touches the host EDMA rings, so
 * an alternative data plane (the NSS firmware fast path) can transmit
 * it instead.
 *
 * The handler runs in the conduit's ndo_start_xmit context (BH, RCU
 * read side) and takes ownership of the skb regardless of its return
 * value, except for NETDEV_TX_BUSY which requeues upstream.
 */

#ifndef __LINUX_SOC_QCOM_QCA_EDMA_H__
#define __LINUX_SOC_QCOM_QCA_EDMA_H__

#include <linux/netdevice.h>

/* PPE port ids carried in the OOB tag; physical ports are 1..6 */
#define QCA_EDMA_REDIRECT_MAX_PORT	7

typedef netdev_tx_t (*qca_edma_tx_redirect_fn)(struct sk_buff *skb, void *ctx);

bool qca_edma_netdev_is_conduit(const struct net_device *netdev);
int qca_edma_port_tx_redirect_set(struct net_device *conduit,
				  unsigned int port,
				  qca_edma_tx_redirect_fn fn, void *ctx);
int qca_edma_port_tx_redirect_clear(struct net_device *conduit,
				    unsigned int port);

#endif /* __LINUX_SOC_QCOM_QCA_EDMA_H__ */
