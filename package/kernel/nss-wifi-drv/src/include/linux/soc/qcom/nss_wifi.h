/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * What a WLAN driver tells the NSS firmware about its rings.
 *
 * The firmware reaps the WLAN hardware's rings itself, so it has to be told
 * where they are. This is the whole of what it needs: the description is
 * filled from whatever the WLAN driver already knows and handed over once,
 * which keeps the firmware driver free of that driver's internals and of the
 * kernel version they change with.
 *
 * Registration is not a handover. The description is kept until something
 * asks for the firmware to be started, because a running core takes the
 * hardware with it and that stays a deliberate act.
 */

#ifndef __LINUX_SOC_QCOM_NSS_WIFI_H__
#define __LINUX_SOC_QCOM_NSS_WIFI_H__

#include <linux/types.h>

#define NSS_WIFI_MAX_TCL_RINGS	4
#define NSS_WIFI_MAX_REO_RINGS	4

/*
 * One hardware ring. Everything here is a property of the ring the WLAN
 * driver allocated; the firmware derives the rest. entry_size is in 32-bit
 * words, and hwreg[1] is the absolute address of the register the firmware
 * moves the ring's pointer through.
 */
struct nss_wifi_ring {
	u8 id;
	u8 dir;
	u32 num_entries;
	u32 entry_size;
	u32 flags;
	u32 base;
	u32 hwreg[2];
};

struct nss_wifi_pdev {
	struct nss_wifi_ring rxdma;
	u8 radio_id;
	u8 lmac_id;
	u8 target_pdev_id;
	u32 num_rx_swdesc;
};

struct nss_wifi_soc {
	u32 dev_base;
	u32 shadow_rd;
	u32 shadow_wr;
	u32 lmac_ring_start;
	u32 target_type;
	u16 tlv_size;
	u16 rx_buf_len;
	u8 num_tcl;
	u8 num_reo;
	struct nss_wifi_ring tcl[NSS_WIFI_MAX_TCL_RINGS];
	struct nss_wifi_ring txcomp[NSS_WIFI_MAX_TCL_RINGS];
	struct nss_wifi_ring reo_dest[NSS_WIFI_MAX_REO_RINGS];
	struct nss_wifi_ring reo_reinject;
	struct nss_wifi_ring rx_rel;
	struct nss_wifi_ring reo_exception;

	/*
	 * The firmware reaps rings holding MSDU-link descriptors it cannot
	 * put back, because the ring that returns one to the idle list is not
	 * among those it is told about. It hands each one back by message,
	 * and this is the way home. Called from softirq, may not sleep.
	 */
	void *priv;
	void (*link_desc_return)(void *priv, const u32 *buf_addr_info);
};

#if IS_REACHABLE(CONFIG_NSS_WIFI_DRV)
int nss_wifi_soc_register(const struct nss_wifi_soc *soc);
void nss_wifi_soc_unregister(void);
int nss_wifi_pdev_register(const struct nss_wifi_pdev *pdev);
int nss_wifi_vdev_register(struct net_device *dev, u8 radio, u32 vdev_id,
			   const u8 *mac, bool ap);
void nss_wifi_vdev_unregister(struct net_device *dev);
int nss_wifi_vdev_tx(int if_num, struct sk_buff *skb);
int nss_wifi_peer_create(u32 vdev_id, const u8 *mac, u16 peer_id,
			 u16 hw_ast_idx, u32 tx_ast_hash);
void nss_wifi_peer_delete(u32 vdev_id, const u8 *mac, u16 peer_id);
int nss_wifi_vdev_security(int if_num, u32 cipher);
#else
static inline int nss_wifi_soc_register(const struct nss_wifi_soc *soc)
{
	return -ENODEV;
}

static inline void nss_wifi_soc_unregister(void)
{
}

static inline int nss_wifi_pdev_register(const struct nss_wifi_pdev *pdev)
{
	return -ENODEV;
}

static inline int nss_wifi_vdev_register(struct net_device *dev, u8 radio,
					 u32 vdev_id, const u8 *mac, bool ap)
{
	return -ENODEV;
}

static inline void nss_wifi_vdev_unregister(struct net_device *dev)
{
}

static inline int nss_wifi_vdev_tx(int if_num, struct sk_buff *skb)
{
	return -ENODEV;
}

static inline int nss_wifi_peer_create(u32 vdev_id, const u8 *mac, u16 peer_id,
				       u16 hw_ast_idx, u32 tx_ast_hash)
{
	return -ENODEV;
}

static inline void nss_wifi_peer_delete(u32 vdev_id, const u8 *mac,
					u16 peer_id)
{
}

static inline int nss_wifi_vdev_security(int if_num, u32 cipher)
{
	return -ENODEV;
}
#endif

#endif /* __LINUX_SOC_QCOM_NSS_WIFI_H__ */
