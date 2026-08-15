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
flowtable. The NSS Wi-Fi path is on by default on IPQ807x. IPQ60xx
radios stay on the host path.

IPQ60xx support is new and lightly tested. The maintainer has no IPQ60xx
hardware, so reports from IPQ60xx users are welcome.

## Images

Prebuilt images for every IPQ807x board, and every IPQ60xx board with an
NSS node in its device tree, are on the
[Qualcommax_NSS_Builder releases](https://github.com/JuliusBairaktaris/Qualcommax_NSS_Builder/releases)
page. The `ppe-nss` releases are the default. The `ppe-nss-mesh` releases
carry the same boards on NSS firmware 11.4, the last line that supports
802.11s mesh. Mesh is only partly offloaded so far.

## Documentation

The [wiki](https://github.com/JuliusBairaktaris/openwrt-ppe-nss/wiki)
covers the offload, SQM, hardware support and known limitations.

## Branches

| Branch | Contents |
|---|---|
| `ppe-nss` | The default. `ppe-flow-offload` plus the NSS Wi-Fi series. |
| `ppe-flow-offload` | [openwrt/openwrt](https://github.com/openwrt/openwrt) `main` plus the PPE flowtable, tc and switchdev offload series ([openwrt/openwrt#24806](https://github.com/openwrt/openwrt/pull/24806); its first commits are [#25700](https://github.com/openwrt/openwrt/pull/25700)). No NSS. |
| `nss-edma-rework` | Archived. The vendor NSS stack (qca-nss-drv, ECM, NSS qdiscs). Final state: tag `archive/nss-edma-rework-final`. |

The NSS Wi-Fi series on `ppe-nss` adds:

1. `qca_edma` and `qca_ppe` changes that let the NSS firmware share the
   EDMA and PPE with the host driver, and the NSS device tree nodes.
2. `nss-wifi-drv` and the ath11k patches that hand the data-path rings
   to the NSS firmware.

The only package taken from the
[nss-packages](https://github.com/JuliusBairaktaris/nss-packages) feed
(branch `edma-nss`) is `nss-firmware`.

## Building

```sh
git clone -b ppe-nss https://github.com/JuliusBairaktaris/openwrt-ppe-nss.git
cd openwrt-ppe-nss
cp feeds.conf.default feeds.conf
echo "src-git nss https://github.com/JuliusBairaktaris/nss-packages.git;edma-nss" >> feeds.conf
./scripts/feeds update -a && ./scripts/feeds install -a
# Apply a seed config:
# Normal Wi-Fi offload:
cat seeds/qualcommax-nss-wifi.seed >> .config
# OR Wi-Fi offload + 802.11s mesh (firmware 11.4):
# cat seeds/qualcommax-nss-wifi-mesh.seed >> .config

make defconfig
make -j$(nproc)
```

Seed configurations are provided in [`seeds/`](seeds/):
- [`qualcommax-nss-wifi.seed`](seeds/qualcommax-nss-wifi.seed): Stock qualcommax (IPQ807x / IPQ60xx) plus `kmod-nss-wifi-drv`, `ATH11K_NSS_WIFI`, and NSS firmware 12.5.
- [`qualcommax-nss-wifi-mesh.seed`](seeds/qualcommax-nss-wifi-mesh.seed): Wi-Fi offload + 802.11s mesh (`wpad-mesh-openssl`) on NSS firmware 11.4.

The `hw_ppe.qos` SQM script and the PPE QoS LuCI page come from feed
patches in the builder; its
[README](https://github.com/JuliusBairaktaris/Qualcommax_NSS_Builder#build-it-yourself)
has the full steps.

The image boots with the PPE offload and the NSS Wi-Fi offload on. The
Wi-Fi offload is chosen before the radios probe and turned off with
`uci set nss-wifi.general.offload=0; uci commit nss-wifi; reboot`. The
flag is cleared at boot and set again two minutes after a bring-up that
succeeds, so a bring-up that hangs the board costs one boot.

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
