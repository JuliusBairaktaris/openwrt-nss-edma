#
# Copyright (C) 2022 OpenWrt.org
#
# This is free software, licensed under the GNU General Public License v2.
# See /LICENSE for more information.
#

include $(TOPDIR)/rules.mk

PKG_NAME:=ath11k-firmware
PKG_SOURCE_DATE:=2024-10-14
PKG_SOURCE_VERSION:=15f050122da5ef5bef2cc8c7c19dfb7f98060a49
PKG_MIRROR_HASH:=e7a89d1570f32552ade539376181494724b5eafb31d00451ff2dcf5a2922a0cb
PKG_RELEASE:=1

PKG_SOURCE_PROTO:=git
PKG_SOURCE_URL:=https://git.codelinaro.org/clo/ath-firmware/ath11k-firmware.git

PKG_LICENSE_FILES:=LICENSE.qca_firmware

PKG_MAINTAINER:=Robert Marko <robimarko@gmail.com>

include $(INCLUDE_DIR)/package.mk

RSTRIP:=:
STRIP:=:

# The IPQ8074 set this board needs exists in no single upstream release, so it is
# re-published as one pinned tarball alongside the other pinned NSS source
# tarballs rather than carried as ~11 MB of binaries in this repository. What is
# in it, so the provenance stays traceable:
#
# All 18 files are the DD-WRT IPQ8074 set, taken as a unit so the firmware and the
# board data it was validated against stay together. The q6/m3 half is
# WLAN.HK.2.12-01460 and was checked byte-for-byte against the set published in
# qosmio/upstream-wifi-fw release 2.12-01460 - they are identical, which confirms
# it is the genuine QCA release rather than a respin. There is no upstream to pin
# instead: neither CLO's ath11k-firmware nor quic/upstream-wifi-fw carries 2.12
# for IPQ8074 (both stop at 2.9.0.1), the qosmio asset carries no IPQ8074 board
# data at all, and CLO's board-2.bin is a different file.
IPQ8074_FW_NAME:=ath11k-fw-ipq8074
IPQ8074_FW_VERSION:=2.12-01460
IPQ8074_FW_SUBDIR:=ath11k-firmware-ipq8074-$(IPQ8074_FW_VERSION)
IPQ8074_FW_SOURCE:=$(IPQ8074_FW_SUBDIR).tar.zst

define Download/$(IPQ8074_FW_NAME)
	FILE:=$(IPQ8074_FW_SOURCE)
	URL:=https://github.com/JuliusBairaktaris/nss-packages/releases/download/nss-source-tarballs
	HASH:=b4241d7fd93dd632c4100ff1088670881e131674fbe650e5bc2f9fd636a027ea
endef

ATH11K_LEGACY_FW_NAME:=ath11k-legacy-firmware
ATH11K_LEGACY_FW_DATE:=2024-03-14
ATH11K_LEGACY_FW_RELEASE:=795809c7
ATH11K_LEGACY_FW_SUBDIR:=$(ATH11K_LEGACY_FW_NAME)-$(ATH11K_LEGACY_FW_DATE)~$(ATH11K_LEGACY_FW_RELEASE)
ATH11K_LEGACY_FW_SOURCE=$(ATH11K_LEGACY_FW_SUBDIR).tar.zst

define Download/$(ATH11K_LEGACY_FW_NAME)
	FILE:=$(ATH11K_LEGACY_FW_SOURCE)
	PROTO:=git
	URL:=https://github.com/quic/upstream-wifi-fw.git
	SOURCE_VERSION:=795809c7041582bd51bdfaa1f548b916ae8d4382
	MIRROR_HASH:=b55bf7204543c7ba87876e84788239b0e47ad62434c5b0d1169526cde36a9daa
	SUBDIR:=$(ATH11K_LEGACY_FW_SUBDIR)
endef
$(eval $(call Download,$(ATH11K_LEGACY_FW_NAME)))
$(eval $(call Download,$(IPQ8074_FW_NAME)))

define Package/ath11k-firmware-default
  SECTION:=firmware
  CATEGORY:=Firmware
  URL:=$(PKG_SOURCE_URL)
  DEPENDS:=
endef

define Package/ath11k-firmware-ipq5018-qcn6122
$(Package/ath11k-firmware-default)
  TITLE:=IPQ5018/QCN6122 ath11k firmware
endef

define Package/ath11k-firmware-ipq6018
$(Package/ath11k-firmware-default)
  TITLE:=IPQ6018 ath11k firmware
endef

define Package/ath11k-firmware-ipq8074
$(Package/ath11k-firmware-default)
  TITLE:=IPQ8074 ath11k firmware
endef

define Build/Clean
	$(call Build/Clean/Default,)

	rm -rf \
		$(BUILD_DIR)/$(ATH11K_LEGACY_FW_SUBDIR) \
		$(BUILD_DIR)/$(IPQ8074_FW_SUBDIR)
endef

define Build/Prepare
	$(call Build/Prepare/Default,)

	$(TAR) -C $(BUILD_DIR) -xf $(DL_DIR)/$(ATH11K_LEGACY_FW_SOURCE)
	mkdir -p $(BUILD_DIR)/$(IPQ8074_FW_SUBDIR)
	$(TAR) -C $(BUILD_DIR)/$(IPQ8074_FW_SUBDIR) -xf $(DL_DIR)/$(IPQ8074_FW_SOURCE)
endef

define Build/Compile

endef

define Package/ath11k-firmware-ipq5018-qcn6122/install
	$(INSTALL_DIR) $(1)/lib/firmware/ath11k/IPQ5018/hw1.0
	$(INSTALL_DATA) \
		$(BUILD_DIR)/$(ATH11K_LEGACY_FW_SUBDIR)/ath11k-firmware/IPQ5018_QCN6122_QCN6122/hw1.0/2.7.0.1/WLAN.HK.2.7.0.1-01744-QCAHKSWPL_SILICONZ-1/*.* \
		$(1)/lib/firmware/ath11k/IPQ5018/hw1.0/
	$(INSTALL_DIR) $(1)/lib/firmware/ath11k/QCN6122/hw1.0
	$(INSTALL_DATA) \
		$(BUILD_DIR)/$(ATH11K_LEGACY_FW_SUBDIR)/ath11k-firmware/IPQ5018_QCN6122_QCN6122/hw1.0/2.7.0.1/WLAN.HK.2.7.0.1-01744-QCAHKSWPL_SILICONZ-1/qcn6122/*.* \
		$(1)/lib/firmware/ath11k/QCN6122/hw1.0/
endef

define Package/ath11k-firmware-ipq6018/install
	$(INSTALL_DIR) $(1)/lib/firmware/IPQ6018
	$(INSTALL_DATA) \
		$(PKG_BUILD_DIR)/IPQ6018/hw1.0/2.5.0.1/WLAN.HK.2.5.0.1-03982-QCAHKSWPL_SILICONZ-3/* \
		$(1)/lib/firmware/IPQ6018/
endef

define Package/ath11k-firmware-ipq8074/install
	$(INSTALL_DIR) $(1)/lib/firmware/IPQ8074
	$(INSTALL_DATA) \
		$(BUILD_DIR)/$(IPQ8074_FW_SUBDIR)/IPQ8074/q6_fw.* \
		$(BUILD_DIR)/$(IPQ8074_FW_SUBDIR)/IPQ8074/m3_fw.* \
		$(BUILD_DIR)/$(IPQ8074_FW_SUBDIR)/IPQ8074/*.bin \
		$(1)/lib/firmware/IPQ8074/
endef

$(eval $(call BuildPackage,ath11k-firmware-ipq5018-qcn6122))
$(eval $(call BuildPackage,ath11k-firmware-ipq6018))
$(eval $(call BuildPackage,ath11k-firmware-ipq8074))
