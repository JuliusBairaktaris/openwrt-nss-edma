# OpenWrt with PPE and NSS offload for IPQ807x and IPQ60xx

OpenWrt main with hardware offload for Qualcomm IPQ807x and IPQ60xx routers
in the `qualcommax` target, such as the Xiaomi AX3600, Redmi AX6, Linksys
MX4300 and MR7350, Dynalink DL-WRX36 and GL.iNet GL-AX1800. It runs
OpenWrt's upstream `qca_edma` and `qca_ppe` ethernet and DSA drivers on
kernel 6.18 and uses only in-kernel offload interfaces: no ECM, no NSS
qdiscs and no vendor NSS host driver.

| Traffic | Engine | Kernel interface |
|---|---|---|
| Routed IPv4/IPv6, NAT, PPPoE, 802.1Q VLAN | PPE | fw4 flowtable (`flow_offloading_hw`) |
| Bridging, VLAN filtering, multicast snooping | PPE | switchdev |
| SQM shaping and queue classes | PPE | tc qdisc and flower offload, DCB |
| Wi-Fi data path of the SoC radios | NSS | `nss-wifi-drv`, ath11k |
| Routed and bridged flows with a Wi-Fi end | NSS | fw4 flowtable, bridge conntrack |
| Routed multicast to Wi-Fi | NSS | kernel multicast routing (MFC) |
| Routed flows over a cellular modem (raw-IP device) | NSS | fw4 flowtable |

The PPE is the packet engine in the SoC's switch; `qca_ppe` programs it
without firmware. `nss-wifi-drv` is a small host driver that boots the NSS
firmware and hands it the ath11k rings. The Wi-Fi offload is on by default
and is turned off with:

```sh
uci set nss-wifi.general.offload='0'; uci commit nss-wifi; reboot
```

The Xiaomi AX3600 is the test board. Other boards are built but not tested
by the maintainer, and IPQ60xx is not tested at all. Reports are welcome.

## Images

Prebuilt sysupgrade images for 41 IPQ807x and 24 IPQ60xx boards are on the
[Qualcommax_NSS_Builder releases](https://github.com/JuliusBairaktaris/Qualcommax_NSS_Builder/releases)
page. The images carry NSS firmware 12.5 and 11.4, the last line that
supports 802.11s mesh, and boot 12.5. Mesh users switch with
`uci set nss-wifi.general.firmware=11.4; uci commit nss-wifi; reboot`; a
config kept from a mesh image switches by itself. Mesh is only partly
offloaded so far.

## Documentation

The [wiki](https://github.com/JuliusBairaktaris/openwrt-ppe-nss/wiki)
covers installing, each part of the offload, SQM, building, bug reports,
limitations and a FAQ.

Coming from an edma-nss image with settings kept? The image does not convert
the old configuration; follow
[Migrating from edma-nss](https://github.com/JuliusBairaktaris/openwrt-ppe-nss/wiki/Migrating-from-edma-nss).

## Branches

| Branch | Contents |
|---|---|
| `ppe-nss` | The default. `ppe-flow-offload` plus the NSS Wi-Fi series. |
| `ppe-flow-offload` | [openwrt/openwrt](https://github.com/openwrt/openwrt) `main` plus the PPE flowtable, tc and switchdev offload ([openwrt/openwrt#24806](https://github.com/openwrt/openwrt/pull/24806); its first commits are [#25700](https://github.com/openwrt/openwrt/pull/25700)). No NSS. |
| `nss-edma-rework` | Archived. The vendor NSS stack (qca-nss-drv, ECM, NSS qdiscs). Final state: tag `archive/nss-edma-rework-final`. |

The NSS Wi-Fi series on `ppe-nss` adds the NSS device tree nodes, the
`qca_edma` and `qca_ppe` changes that let the NSS firmware share the EDMA
and PPE with the host, `nss-wifi-drv`, and the ath11k patches that hand the
data-path rings to the firmware. The only package taken from the
[nss-packages](https://github.com/JuliusBairaktaris/nss-packages) feed
(branch `edma-nss`) is `nss-firmware`.

## Building

```sh
git clone -b ppe-nss https://github.com/JuliusBairaktaris/openwrt-ppe-nss.git
cd openwrt-ppe-nss
cp feeds.conf.default feeds.conf
echo "src-git nss https://github.com/JuliusBairaktaris/nss-packages.git;edma-nss" >> feeds.conf
./scripts/feeds update -a && ./scripts/feeds install -a
cat seeds/qualcommax-nss-wifi.seed >> .config
make defconfig
make -j"$(nproc)"
```

[`seeds/qualcommax-nss-wifi.seed`](seeds/qualcommax-nss-wifi.seed) selects
`kmod-nss-wifi-drv`, `ATH11K_NSS_WIFI`, both NSS firmware lines and
`wpad-openssl`, which supports 802.11s mesh.

`hw_ppe.qos` for SQM and the PPE QoS LuCI page come from a feed patch and
a luci fork in the builder. Its
[README](https://github.com/JuliusBairaktaris/Qualcommax_NSS_Builder#build-it-yourself)
and the wiki's [Building](https://github.com/JuliusBairaktaris/openwrt-ppe-nss/wiki/Building)
page build the exact release images.

`ppe-nss` is rebased often. Update with
`git fetch origin && git reset --hard origin/ppe-nss`, not `git pull`.

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

GPL-2.0, see [COPYING](COPYING). The NSS firmware keeps its own license.
