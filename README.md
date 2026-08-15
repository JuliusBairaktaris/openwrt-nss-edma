# OpenWrt with PPE and NSS hardware offload for IPQ807x and IPQ60xx

This is OpenWrt main with hardware offload for IPQ807x and IPQ60xx
(IPQ6018) routers, such as the Xiaomi AX3600, Redmi AX6, Linksys MX4300
and MR7350, and GL.iNet GL-AX1800. It runs on the upstream `qca_edma` and
`qca_ppe` ethernet and DSA drivers on kernel 6.18 and uses only in-kernel
offload interfaces: no ECM, no NSS qdiscs and no vendor NSS host driver.

Wired traffic is offloaded by the PPE: routed IPv4/IPv6 and NAT through
the fw4 flowtable (`flow_offloading_hw`), PPPoE and 802.1Q VLAN,
bridging and multicast snooping through switchdev, and SQM through tc
qdisc offload (tbf/ets). The NSS firmware is driven by `nss-wifi-drv`, a
small host driver that offloads ath11k Wi-Fi flows from the same
flowtable. The NSS Wi-Fi path is on by default.

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

1. The PPE flowtable, tc and switchdev offload series
   ([openwrt/openwrt#24806](https://github.com/openwrt/openwrt/pull/24806)).
2. `qca_edma` and `qca_ppe` changes that let the NSS firmware share the
   EDMA and PPE with the host driver, and the NSS device tree nodes.
3. `nss-wifi-drv` and the ath11k patches that hand the data-path rings
   to the NSS firmware.

The only package taken from the
[nss-packages](https://github.com/JuliusBairaktaris/nss-packages) feed
(branch `edma-nss`) is `nss-firmware`.

## Building

```sh
git clone -b nss-edma-rework https://github.com/JuliusBairaktaris/openwrt-nss-edma.git
cd openwrt-nss-edma
cp feeds.conf.default feeds.conf
echo "src-git nss https://github.com/JuliusBairaktaris/nss-packages.git;edma-nss" >> feeds.conf
./scripts/feeds update -a && ./scripts/feeds install -a
make menuconfig   # qualcommax/ipq807x or ipq60xx, kmod-nss-wifi-drv
make -j$(nproc)
```

The image boots on the host datapath with the PPE offload. The NSS Wi-Fi
offload is chosen once per boot, before the radios probe:
`uci set nss-wifi.general.offload=1; uci commit nss-wifi; reboot`. The
flag is consumed at boot, so the next boot is back on the host path.

## Acknowledgements

- [Christian Marangi (Ansuel)](https://github.com/Ansuel) for the upstream
  EDMA and PPE drivers.
- [Robert Marko (robimarko)](https://github.com/robimarko) for maintaining the
  OpenWrt qualcommax target.
- [qosmio](https://github.com/qosmio/openwrt-ipq) for the NSS firmware
  packaging the feed builds on.

## Support

[GitHub Sponsors](https://github.com/sponsors/JuliusBairaktaris) or
[PayPal](https://paypal.me/JuliusBairaktaris).

## License

GPL-2.0, see [LICENSE](LICENSE). The NSS firmware keeps its own license.
