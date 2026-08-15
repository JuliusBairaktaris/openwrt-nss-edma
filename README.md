# OpenWrt with NSS hardware offload for IPQ807x and IPQ60xx

This is OpenWrt main with Qualcomm NSS hardware offload for IPQ807x and
IPQ60xx (IPQ6018) routers, such as the Xiaomi AX3600, Redmi AX6, Linksys
MX4300 and MR7350, and GL.iNet GL-AX1800. The NSS firmware runs on top of
the upstream `qca_edma` and `qca_ppe` ethernet and DSA drivers on kernel
6.18. It does not use the out-of-tree `qca-nss-dp` and `qca-ssdk` drivers.

Offloaded: IPv4 NAT and IPv6 routing through ECM, PPPoE, 802.1Q VLAN,
bridging, multicast snooping, the NSS qdiscs for SQM, and ath11k Wi-Fi
offload. 802.11s mesh offload needs the 11.4 NSS firmware (the mesh
images). The full support matrix is in the wiki.

IPQ60xx support is new and lightly tested. The maintainer has no IPQ60xx
hardware, so reports from IPQ60xx users are welcome.

## Images

Prebuilt images for every IPQ807x board, and every IPQ60xx board with an
NSS node in its device tree, are on the
[Qualcommax_NSS_Builder releases](https://github.com/JuliusBairaktaris/Qualcommax_NSS_Builder/releases)
page. IPQ60xx images are in the `ipq60xx-1g` and `ipq60xx-512m` groups.
Each build comes in a default and a mesh flavour.

## Documentation

The [wiki](https://github.com/JuliusBairaktaris/openwrt-nss-edma/wiki)
covers the architecture, runtime operation, SQM, hardware support and
known limitations.

## Branch layout

`nss-edma-rework` is
[openwrt/openwrt](https://github.com/openwrt/openwrt) `main` plus a
series on top:

1. `qca_edma` and `qca_ppe` changes that let the NSS firmware share the
   EDMA and PPE with the host driver.
2. NSS device tree nodes for IPQ807x and IPQ6018.
3. Kernel support patches for ECM, the NSS qdiscs and qca-mcs.
4. The `kmod-qca-ppe-nss` glue module and the NSS runtime tools.
5. ath11k and mac80211 NSS Wi-Fi offload.

The NSS packages (driver, ECM, qdiscs, firmware) are in the
[nss-packages](https://github.com/JuliusBairaktaris/nss-packages) feed,
branch `edma-nss`.

## Building

```sh
git clone -b nss-edma-rework https://github.com/JuliusBairaktaris/openwrt-nss-edma.git
cd openwrt-nss-edma
cp feeds.conf.default feeds.conf
echo "src-git nss https://github.com/JuliusBairaktaris/nss-packages.git;edma-nss" >> feeds.conf
./scripts/feeds update -a && ./scripts/feeds install -a
make menuconfig   # qualcommax/ipq807x or ipq60xx, select the NSS packages
make -j$(nproc)
```

Without the nss feed, `ATH11K_NSS_SUPPORT` has an unmet dependency and
menuconfig fails.

The image always boots on the host datapath first. The `nss` service then
starts the offload. With `uci set nss.general.enabled=0` the router runs
as stock OpenWrt and no NSS module is loaded.

## Acknowledgements

- [Christian Marangi (Ansuel)](https://github.com/Ansuel) for the upstream
  EDMA and PPE drivers.
- [Robert Marko (robimarko)](https://github.com/robimarko) for maintaining the
  OpenWrt qualcommax target.
- [qosmio](https://github.com/qosmio/openwrt-ipq) for the NSS packaging and
  firmware tarballs the feed builds on.
- Qualcomm and CodeLinaro for the open-source NSS host components.

## Support

[GitHub Sponsors](https://github.com/sponsors/JuliusBairaktaris) or
[PayPal](https://paypal.me/JuliusBairaktaris).

## License

GPL-2.0, see [LICENSE](LICENSE). The NSS components keep their upstream
licenses.
