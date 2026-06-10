# OpenWrt with NSS offload on the upstream EDMA stack

This tree runs **Qualcomm NSS hardware offloading** (the dual UBI32
network cores in IPQ807x SoCs) on top of **OpenWrt main's upstream
`qca_edma`/`qca_ppe` ethernet drivers** from
[openwrt/openwrt#22381](https://github.com/openwrt/openwrt/pull/22381) —
not on the vendor `qca-nss-dp`/`qca-ssdk` driver stack that all other
NSS builds use. As far as we know it is the first NSS integration that
keeps the upstream drivers.

**Measured on a Xiaomi AX3600** (IPQ8071A, 512 MB, PPPoE uplink):

| | Host path | NSS offloaded |
|---|---|---|
| NAT/PPPoE routing @ ~310 Mbit/s | ~42 % of one core (softirq) | **99.7 % CPU idle** |
| SQM shaping @ 285 Mbit | (CPU-bound on this device class) | **99 % idle, 16 ms RTT under full load — zero bufferbloat** |

📖 **Documentation lives in the
[project wiki](https://github.com/JuliusBairaktaris/openwrt-nss-edma/wiki)**:
architecture, firmware and source-pin rationale, runtime operation,
SQM, hardware support, development notes, limitations. It is written
to be self-contained for future contributors.

## Layout of this branch (`nss-edma-rework`)

1. [openwrt/openwrt](https://github.com/openwrt/openwrt) `main` — the
   upstream base.
2. The 14 commits of
   [PR #22381](https://github.com/openwrt/openwrt/pull/22381)
   (Ansuel's EDMA/PPE driver rework), applied verbatim.
3. The integration series (~11 commits): ramoops crash forensics, NSS
   device-tree nodes for every IPQ807x-family board, two `qca_edma`
   hardening/hook commits, per-port firmware VSIs in `qca_ppe`, the
   `kmod-qca-ppe-nss` glue module, the ECM and NSS-qdisc kernel
   patches, and iproute2 `tc` support for the NSS qdiscs.

The NSS packages (driver, ECM, qdisc/PPPoE managers, firmware, SQM
script) live in the companion feed
**[JuliusBairaktaris/nss-packages](https://github.com/JuliusBairaktaris/nss-packages)**,
branch `edma-nss`.

## Prebuilt images

**[Qualcommax_NSS_Builder](https://github.com/JuliusBairaktaris/Qualcommax_NSS_Builder)**
builds this tree (with the `nss-packages` feed, a hardened toolchain
and sensible defaults) automatically whenever either repository moves —
grab the latest `edma-nss-*` tag from its
[Releases](https://github.com/JuliusBairaktaris/Qualcommax_NSS_Builder/releases)
page.

## Quick start

```sh
git clone -b nss-edma-rework https://github.com/JuliusBairaktaris/openwrt-nss-edma.git
cd openwrt-nss-edma
echo "src-link nss /path/to/nss-packages" >> feeds.conf
./scripts/feeds update -a && ./scripts/feeds install -a
make menuconfig   # target qualcommax/ipq807x; select the NSS packages;
                  # NSS_MEM_PROFILE_MEDIUM for 512 MB boards!
make -j$(nproc)
```

The resulting image boots as a completely normal OpenWrt system —
**nothing NSS-related starts at boot, by design**. The NSS data plane
is brought up explicitly at runtime; see
[Runtime Operation](https://github.com/JuliusBairaktaris/openwrt-nss-edma/wiki/Runtime-Operation)
for the sequence and the safety rules. A plain reboot always returns
to the stock host-only stack.

## NSS offload support matrix

What the firmware data plane accelerates on this stack (whole IPQ807x
family). Legend: ✅ offloaded & validated · 🟨 supported in code, opt-in
and not validated here · ⬜ deliberately not carried (software path is
used) · ❌ not available on this platform/firmware.

| Feature | IPQ807x | Notes |
|---|:---:|---|
| IPv4 NAT / routing | ✅ | ECM, line rate, host ~idle |
| IPv6 routing | ✅ | ECM |
| PPPoE (incl. over 802.1Q VLAN) | ✅ | validated on a PPPoE/VLAN WAN |
| 802.1Q VLAN | ✅ | ECM VLAN-tagged flows |
| SQM shaper (nsstbl + nssfq_codel) | ✅ | `nss-edma.qos`; zero-bufferbloat verified |
| Ingress shaping (IGS / nssmirred) | ✅ | `act_nssmirred` → ifb |
| DSCP / mark classification | ✅ | ECM DSCP + mark classifiers |
| CoDel ECN marking | ❌ | 12.5 firmware does not ECN-mark (verified) |
| Wi-Fi AP (wifili) | ✅ | both radios (QCN5024 + QCN5054) |
| Wi-Fi STA | 🟨 | wifili path present; AP is what's validated |
| Wi-Fi WDS | 🟨 | not validated |
| Wi-Fi mesh | ❌ | needs NSS fw 11.4 (this tree ships 12.5); host mesh works |
| Wi-Fi AP-VLAN | ❌ | broken in the ath11k driver |
| Bridge (same-subnet L2) | ⬜ | software bridge; `nss-bridge-mgr` deferred (fw one-port-per-VSI) |
| Multicast (routed / bridged) | ⬜ | `ECM_MULTICAST` not built; fw mc-connection path unused |
| GRE | 🟨 | ECM support builds with `kmod-gre`; not in the default config |
| MAP-T / DS-Lite | 🟨 | needs `kmod-nat46` |
| 6RD / IPIP6 (SIT) | 🟨 | needs `kmod-sit` / `kmod-ip6-tunnel` |
| VXLAN | 🟨 | needs `kmod-vxlan` |
| OVS bridge | 🟨 | needs `kmod-qca-ovsmgr` |
| MACVLAN | 🟨 | kernel patch carried; needs `kmod-macvlan` |
| L2TPv2 / PPTP | ⬜ | ECM interface off — those kernel hooks are not ported |
| Bonding / LAG | ⬜ | kernel bonding hooks not carried |
| IPsec (ESP) | ❌ | not viable on IPQ807x; `nss-crypto`/`cfi` not carried |
| TLS / DTLS / CAPWAP | ❌ | not supported (matches the vendor matrix) |

All IPQ807x-family boards carry the NSS device-tree nodes; per-board
validation reports are the open item. A plain reboot always returns to
the stock host-only stack — nothing NSS starts at boot.

## Acknowledgements

- [Ansuel / Christian Marangi](https://github.com/Ansuel) — the
  upstream EDMA/PPE driver rework (PR #22381) this is built on.
- [qosmio](https://github.com/qosmio/openwrt-ipq) — the community NSS
  builds whose packaging and kernel-compatibility work the package
  feed is derived from, and the prepackaged firmware tarballs.
- Qualcomm/CodeLinaro for the open-source NSS host components.

## License

OpenWrt is licensed under GPL-2.0; see [LICENSE](LICENSE). The NSS
vendor components retain their respective upstream licenses.

---

*This is a development fork. For OpenWrt itself, see
[openwrt/openwrt](https://github.com/openwrt/openwrt).*
