include $(TOPDIR)/rules.mk

PKG_NAME:=ppe-help
PKG_VERSION:=1
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-only
PKG_MAINTAINER:=Julius Bairaktaris <julius@bairaktaris.de>

include $(INCLUDE_DIR)/package.mk

define Package/ppe-help
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=Diagnostic report for the PPE and NSS offload
  DEPENDS:=@TARGET_qualcommax
  PKGARCH:=all
endef

define Package/ppe-help/description
Collects one paste-ready report of the Ethernet, switch, PPE flow
offload, NSS Wi-Fi offload, SQM, WAN and system state, with secrets,
public addresses and most of each MAC address removed. Prints a hint
for each known misconfiguration it finds. Reads state only.
endef

define Build/Compile
endef

define Package/ppe-help/install
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) ./files/ppe-help $(1)/usr/sbin/ppe-help
endef

$(eval $(call BuildPackage,ppe-help))
