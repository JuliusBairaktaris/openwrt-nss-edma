include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=nss-wifi-drv
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-only

PKG_CONFIG_DEPENDS:=CONFIG_NSS_FIRMWARE_VERSION_11_4

include $(INCLUDE_DIR)/package.mk

define KernelPackage/nss-wifi-drv
  SECTION:=kernel
  CATEGORY:=Kernel modules
  SUBMENU:=Network Devices
  TITLE:=NSS firmware host driver for the Wi-Fi data plane
  DEPENDS:=@(TARGET_qualcommax_ipq807x||TARGET_qualcommax_ipq60xx) \
	+nss-firmware +kmod-nft-bridge
  FILES:=$(PKG_BUILD_DIR)/nss-wifi-drv.ko
endef

define KernelPackage/nss-wifi-drv/conffiles
/etc/config/nss-wifi
endef

define KernelPackage/nss-wifi-drv/description
  Boots the NSS firmware and speaks its message protocol, so the Wi-Fi
  data plane runs on the NSS cores instead of the host. Carries only what
  a Wi-Fi offload needs: the wired ports belong to qca_edma and qca_ppe.

  The module does not autoload: the nss-wifi service loads it and boots a
  core when nss-wifi.general.offload is 1, the default. The wired ports
  never depend on it.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	+$(KERNEL_MAKE) M="$(PKG_BUILD_DIR)" \
		CONFIG_NSS_FIRMWARE_VERSION_11_4=$(CONFIG_NSS_FIRMWARE_VERSION_11_4) \
		modules
endef

# The WLAN driver describes its rings through this, and is built by another
# package, so the description has to be reachable from the staging tree.
define Build/InstallDev
	$(INSTALL_DIR) $(1)/usr/include/nss-wifi-drv/linux/soc/qcom
	$(CP) ./src/include/linux/soc/qcom/nss_wifi.h \
		$(1)/usr/include/nss-wifi-drv/linux/soc/qcom/
endef

define KernelPackage/nss-wifi-drv/install
	$(INSTALL_DIR) $(1)/etc/config $(1)/etc/init.d
	$(INSTALL_CONF) ./files/nss-wifi.config $(1)/etc/config/nss-wifi
	$(INSTALL_BIN) ./files/nss-wifi.init $(1)/etc/init.d/nss-wifi
endef

$(eval $(call KernelPackage,nss-wifi-drv))
