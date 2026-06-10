# OpenWrt with Qualcomm NSS hardware offload on the upstream EDMA driver

OpenWrt for **IPQ807x** (Qualcomm IPQ8074 / IPQ8071A) that runs **NSS network
offload** — the two UBI32 packet-processing cores in the SoC — on top of OpenWrt
main's **upstream `qca_edma` / `qca_ppe` ethernet and DSA drivers** from
[openwrt/openwrt#22381](https://github.com/openwrt/openwrt/pull/22381).

Every other NSS build replaces the ethernet stack with Qualcomm's out-of-tree
`qca-nss-dp` + `qca-ssdk` drivers. This tree keeps the upstream drivers and
attaches the NSS firmware to them through a small glue module — the first NSS
integration to do so.

Validated on the **Xiaomi AX3600** (IPQ8071A, 512 MB, PPPoE uplink):

| Workload | Host path | NSS offloaded |
|---|---|---|
| NAT + PPPoE routing @ ~310 Mbit/s | ~42 % of one core (softirq) | **99.7 % CPU idle** |
| SQM shaping @ 285 Mbit | CPU-bound on this class of SoC | **99 % idle, 16 ms RTT under full load — no bufferbloat** |

## How traffic flows: host path vs NSS offload

After a plain reboot the router is **stock OpenWrt** — the CPU forwards every
packet. The NSS data plane is armed explicitly at runtime; once it is up the
firmware forwards established flows in hardware and the CPU only sees the first
packet of each flow plus exceptions (broadcast/multicast, locally-bound, new
connections). A reboot always returns to the host-only stack.

```mermaid
flowchart TB
    NIC(["LAN / WAN ports"])

    subgraph HOST["Host path — stock OpenWrt, the default after every reboot"]
      direction LR
      H1["qca_edma<br/>conduit netdev"] --> H2["Linux datapath<br/>bridge · conntrack · NAT<br/>routing · PPPoE · qdisc"] --> H3{{"CPU softirq<br/>— every packet —"}} --> H1
    end

    subgraph OFF["NSS offload path — armed at runtime, reverts on reboot"]
      direction LR
      N1["PPE classifier +<br/>NSS firmware cores"] --> N2["NAT · PPPoE · VLAN<br/>L2 bridge · SQM shaper<br/>— every packet, in hardware —"]
      N1 -. "miss / exception" .-> N3["ECM on host<br/>installs the flow rule<br/>on the first packet"]
      N3 -. "flow rule" .-> N1
    end

    NIC --> HOST
    NIC --> OFF
```

## Documentation

The [project wiki](https://github.com/JuliusBairaktaris/openwrt-nss-edma/wiki)
covers the architecture, the firmware and source-pin rationale, the runtime
bring-up sequence and its safety rules, SQM, hardware support and the
limitations. New to NSS offload? Start with
[NSS Offload Explained](https://github.com/JuliusBairaktaris/openwrt-nss-edma/wiki/NSS-Offload-Explained),
which builds the concept up from scratch.

## Branch layout (`nss-edma-rework`)

The branch layers cleanly on upstream:

1. [openwrt/openwrt](https://github.com/openwrt/openwrt) `main`.
2. The commits of [PR #22381](https://github.com/openwrt/openwrt/pull/22381)
   (Ansuel's EDMA/PPE driver rework), applied verbatim.
3. The NSS integration series: ramoops crash forensics; the NSS device-tree
   nodes for the IPQ807x boards; the `qca_edma` shared-EDMA hardening and TX/RX
   redirect hooks; per-port firmware VSIs and bridge-mgr exports in `qca_ppe`;
   the `kmod-qca-ppe-nss` glue module (with the `qca-nss-drv` probe gate); the
   ECM and NSS-qdisc kernel support patches; iproute2 `tc` support for the NSS
   qdiscs; the qca-mcs multicast snooping hooks (`0971`); the ath11k/mac80211
   Wi-Fi-offload patches; and staging for nat46/MAP-T, macvlan (`0962`) and
   vxlan-fdb (`0972`).

The NSS packages (driver, ECM, qdisc/PPPoE managers, firmware, SQM script) live
in the companion feed
**[nss-packages](https://github.com/JuliusBairaktaris/nss-packages)**, branch
`edma-nss`.

## Prebuilt images

**[Qualcommax_NSS_Builder](https://github.com/JuliusBairaktaris/Qualcommax_NSS_Builder)**
builds this tree and the feed automatically whenever either moves. Grab the
latest `edma-nss-*` tag from its
[Releases](https://github.com/JuliusBairaktaris/Qualcommax_NSS_Builder/releases).
Building from source is only needed to change something.

## Quick start

```sh
git clone -b nss-edma-rework https://github.com/JuliusBairaktaris/openwrt-nss-edma.git
cd openwrt-nss-edma

# Start from the stock feeds and add the NSS feed. It provides kmod-qca-nss-drv
# etc.; without it ATH11K_NSS_SUPPORT has an unmet dependency and menuconfig
# aborts with a "recursive dependency".
cp feeds.conf.default feeds.conf
echo "src-git nss https://github.com/JuliusBairaktaris/nss-packages.git;edma-nss" >> feeds.conf

./scripts/feeds update -a && ./scripts/feeds install -a
./scripts/feeds list -r nss | grep -q qca-nss-drv && echo "nss feed OK"

make menuconfig   # target qualcommax/ipq807x; select the NSS packages;
                  # NSS_MEM_PROFILE_MEDIUM for 512 MB boards
make -j$(nproc)
```

The image boots as a completely normal OpenWrt system — **nothing NSS-related
starts at boot, by design**. The data plane is brought up explicitly at runtime;
see [Runtime Operation](https://github.com/JuliusBairaktaris/openwrt-nss-edma/wiki/Runtime-Operation)
for the sequence and the safety rules.

## NSS offload support matrix

What the firmware data plane accelerates on this stack (whole IPQ807x family).
Legend: ✅ offloaded & validated · 🟨 supported in code, opt-in, not validated
here · ⬜ deliberately not carried (software path is used) · ❌ not available on
this platform/firmware.

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
| Bridge (wired LAN, same-subnet L2) | ✅ | `nss-bridge-mgr`; firmware hardware-bridges the wired ports (host idle). Wi-Fi members stay host-side until Wi-Fi offload |
| Inter-subnet routing (two subnets on one bridge) | ✅ | same-bridge hairpin route `lan1→lan2` accelerated (`accel_mode=2`, host flat), no config change |
| Multicast (same-subnet / bridged) | ✅ | `qca-mcs` snooping; PPE hardware-bridges to snooped members, host flat |
| Multicast (routed across subnets) | 🟨 | ECM `mc_create` path + kernel ipmr hooks built; needs a multicast-routing daemon (igmpproxy/smcroute) and a two-VIF topology |
| GRE | 🟨 | ECM support builds with `kmod-gre`; not in the default config |
| MAP-T / DS-Lite | 🟨 | needs `kmod-nat46` |
| 6RD / IPIP6 (SIT) | 🟨 | needs `kmod-sit` / `kmod-ip6-tunnel` |
| VXLAN | 🟨 | needs `kmod-vxlan` |
| OVS bridge | ⬜ | `nss-bridge-mgr` OVS path compiled out; would need `kmod-qca-ovsmgr` |
| MACVLAN | 🟨 | kernel patch carried; needs `kmod-macvlan` |
| L2TPv2 / PPTP | ⬜ | ECM interface off — those kernel hooks are not ported |
| Bonding / LAG | ⬜ | kernel bonding hooks not carried |
| IPsec (ESP) | ❌ | not viable on IPQ807x; `nss-crypto`/`cfi` not carried |
| TLS / DTLS / CAPWAP | ❌ | not supported (matches the vendor matrix) |

All IPQ807x-family boards carry the NSS device-tree nodes; per-board validation
reports are the open item.

## Acknowledgements

- [Ansuel / Christian Marangi](https://github.com/Ansuel) — the upstream
  EDMA/PPE driver rework (PR #22381) this is built on.
- [qosmio](https://github.com/qosmio/openwrt-ipq) — the community NSS builds
  whose packaging and kernel-compatibility work the package feed derives from,
  and the prepackaged firmware tarballs.
- Qualcomm / CodeLinaro for the open-source NSS host components.

## Support the project

This is an unpaid, single-maintainer effort. If this work is useful to you,
consider chipping in — it goes toward IPQ807x development and hardware to start
looking into **IPQ50xx** and **IPQ60xx** next.

- **[GitHub Sponsors](https://github.com/sponsors/JuliusBairaktaris)** — zero-fee, GitHub-native
- **[PayPal](https://paypal.me/JuliusBairaktaris)** — one-off donations

Thank you!

## License

OpenWrt is licensed under GPL-2.0; see [LICENSE](LICENSE). The NSS vendor
components retain their respective upstream licenses.

---

*A development fork. For OpenWrt itself, see
[openwrt/openwrt](https://github.com/openwrt/openwrt).*
