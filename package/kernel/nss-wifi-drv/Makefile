include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=nss-wifi-drv
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-only

include $(INCLUDE_DIR)/package.mk

define KernelPackage/nss-wifi-drv
  SECTION:=kernel
  CATEGORY:=Kernel modules
  SUBMENU:=Network Devices
  TITLE:=NSS firmware host driver for the Wi-Fi data plane
  DEPENDS:=@TARGET_qualcommax +nss-firmware
  FILES:=$(PKG_BUILD_DIR)/nss-wifi-drv.ko
endef

define KernelPackage/nss-wifi-drv/description
  Boots the NSS firmware and speaks its message protocol, so the Wi-Fi
  data plane runs on the NSS cores instead of the host. Carries only what
  a Wi-Fi offload needs: the wired ports belong to qca_edma and qca_ppe.

  Nothing autoloads and nothing runs until a core is booted by hand, so an
  image carrying this module still boots the host ethernet stack.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	+$(KERNEL_MAKE) M="$(PKG_BUILD_DIR)" modules
endef

# The WLAN driver describes its rings through this, and is built by another
# package, so the description has to be reachable from the staging tree.
define Build/InstallDev
	$(INSTALL_DIR) $(1)/usr/include/nss-wifi-drv/linux/soc/qcom
	$(CP) ./src/include/linux/soc/qcom/nss_wifi.h \
		$(1)/usr/include/nss-wifi-drv/linux/soc/qcom/
endef

$(eval $(call KernelPackage,nss-wifi-drv))
