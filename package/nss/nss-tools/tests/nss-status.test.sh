#!/usr/bin/env bash
# Tests for nss-status, run against a synthetic /sys tree (NSS_STATUS_ROOT) with
# the external commands it shells out to stubbed on PATH. No hardware, no root.
#
# Run with: bash package/nss/nss-tools/tests/nss-status.test.sh
#
# The port table's fixture is generated from the format string in the glue
# driver rather than pasted from a device. A pasted line is a snapshot of one
# build: add a field to the driver and the fixture still describes the old
# layout, so a parser reading the wrong column keeps passing. Generating it
# means the day a field is inserted, this fixture grows it too and any parser
# that located a counter by column number fails here instead of on the router
# - which is how a field insert once turned every offload share into 100%.

set -euo pipefail

here="$(cd -- "$(dirname -- "$0")" && pwd)"
files="$here/../files"
glue_src="$here/../../../kernel/qca-ppe-nss/src/qca_ppe_nss.c"

fail=0
check() { # check <name> <want> <got>
	if [ "$2" = "$3" ]; then
		printf 'PASS  %s\n' "$1"
	else
		printf 'FAIL  %s\n  want: %s\n  got:  %s\n' "$1" "$2" "$3"
		fail=1
	fi
}
skip() { printf 'SKIP  %s\n        %s\n' "$1" "$2"; }

# ---- the driver's port line, and a value per field -------------------------

# The seq_printf whose first literal is the port line; keep the keys in the
# order the driver prints them.
port_fmt="$(sed -n 's/.*"\(if_num %d: netdev=[^"]*\)\\n".*/\1/p' "$glue_src")"
[ -n "$port_fmt" ] || { echo "FAIL  cannot find the if_num format in $glue_src"; exit 1; }

port_keys="$(printf '%s\n' "$port_fmt" | tr ' ' '\n' | sed -n 's/^\([a-z_]*\)=%.*/\1/p')"
for want_key in netdev armed overridden started fw_vsi tx_redirect_pkts \
		tx_busy tx_dropped tx_ungranted fw_link rx_fw_pkts; do
	printf '%s\n' "$port_keys" | grep -qx "$want_key" ||
		{ echo "FAIL  driver no longer prints $want_key"; exit 1; }
done

# Distinct value per key, so a counter read from the wrong column cannot
# coincidentally match the right one. Chosen to be hostile to a column slip:
# fw_vsi is a small number that reads like a plausible packet count, which is
# exactly what made the original mistake invisible.
port_line() { # port_line <ifnum> <netdev> <started> <tx_redirect> <rx_fw>
	local out="if_num $1:" k
	for k in $port_keys; do
		case "$k" in
		netdev)           out="$out netdev=$2" ;;
		armed|overridden) out="$out $k=1" ;;
		started)          out="$out started=$3" ;;
		tx_redirect_pkts) out="$out tx_redirect_pkts=$4" ;;
		rx_fw_pkts)       out="$out rx_fw_pkts=$5" ;;
		fw_vsi)           out="$out fw_vsi=7" ;;
		# Distinct from tx_busy, which sits next to it: the whole point of
		# this counter is that it disagrees with tx_redirect_pkts, so a
		# reader that picked up either neighbour has to fail here.
		tx_dropped)       out="$out tx_dropped=9" ;;
		fw_link)          out="$out fw_link=1" ;;
		tx_ungranted)     out="$out tx_ungranted=11" ;;
		*)                out="$out $k=3" ;;
		esac
	done
	printf '%s\n' "$out"
}

# ---- synthetic root --------------------------------------------------------

root="$(mktemp -d)"
trap 'rm -rf "$root"' EXIT

mkdir -p "$root/lib/nss" "$root/sys/kernel/debug/qca-ppe-nss" \
	 "$root/sys/kernel/debug/qca-nss-drv/stats" \
	 "$root/sys/kernel/debug/ecm/ecm_nss_ipv4" \
	 "$root/sys/kernel/debug/ecm/ecm_nss_ipv6" \
	 "$root/sys/module/ecm" "$root/bin"
cp "$files/functions.sh" "$root/lib/nss/functions.sh"

glue="$root/sys/kernel/debug/qca-ppe-nss"
drv="$root/sys/kernel/debug/qca-nss-drv/stats"

echo 0x3c > "$glue/fw_mask"
printf '\tn2h_rx_pkts = 5000\n' > "$drv/n2h"
printf 'Core 0:\n Min 1%% Avg 4%% Max 9%%\n' > "$drv/cpu_load_ubi"
echo 116 > "$root/sys/kernel/debug/ecm/ecm_nss_ipv4/accelerated_count"
echo 48 > "$root/sys/kernel/debug/ecm/ecm_nss_ipv6/accelerated_count"

netdev() { # netdev <name> <tx_packets> <rx_packets>
	mkdir -p "$root/sys/class/net/$1/statistics"
	echo "$2" > "$root/sys/class/net/$1/statistics/tx_packets"
	echo "$3" > "$root/sys/class/net/$1/statistics/rx_packets"
	echo 1 > "$root/sys/class/net/$1/carrier"
}
netdev wan 1000 2000
netdev lan1 500 400

{ port_line 2 wan 1 250 500; port_line 3 lan1 1 0 0
  port_line 6 - 0 0 0; } > "$glue/status"

# Externals the report shells out to; their answers are per-test, via files.
# The uci stub honours show/get the way uci does, quotes included: 'show'
# quotes option values and 'get' does not, and a stub that ignored the
# difference would let a caller that forgot to unquote pass here.
stub() { printf '#!/bin/sh\n%s\n' "$2" > "$root/bin/$1"; chmod +x "$root/bin/$1"; }
stub tc  '[ $# -gt 0 ] || { echo STUB; exit 0; }
cat "$TC_OUT" 2>/dev/null || true'
stub uci 'for a in "$@"; do last=$a; done
case " $* " in
*" show "*) grep "^$last\." "$UCI_OUT" 2>/dev/null ;;
*" get "*)  sed -n "s|^$last=||p" "$UCI_OUT" 2>/dev/null | tr -d "'\''" ;;
esac
true'
stub nft   'true'
stub dmesg 'true'
: > "$root/tc.out"
: > "$root/uci.out"
export TC_OUT="$root/tc.out" UCI_OUT="$root/uci.out"

# The router runs these under busybox, which is a different shell and a
# different awk from the one a workstation reaches for. NSS_TEST_SH lets the
# whole suite re-run against busybox applets rather than trusting that the two
# agree - they do not, on exactly the string handling these parsers lean on.
run() {
	NSS_STATUS_ROOT="$root" PATH="$root/bin:$PATH" \
		"${NSS_TEST_SH:-sh}" "$files/nss-status" "$@"
}

# A busybox built as a standalone shell runs its own applets and never consults
# PATH, so a stub cannot stand in for one it happens to ship - tc, here.
# OpenWrt builds busybox without either, so this is a limit of the runner's
# busybox and not of the script. Say so out loud: a case quietly dropped from a
# run reads as a case that passed, which is the failure this whole file exists
# to stop.
stubs_honoured=1
PATH="$root/bin:$PATH" "${NSS_TEST_SH:-sh}" -c 'tc' 2>/dev/null | \
	grep -q '^STUB$' || stubs_honoured=0
json_num() { printf '%s' "$1" | tr ',{' '\n\n' | sed -n "s/^\"$2\"://p" | head -1; }
port_field() { # port_field <json> <ifnum> <key>
	printf '%s' "$1" | tr '{' '\n' | grep "\"ifnum\":$2," |
		tr ',' '\n' | sed -n "s/^\"$3\"://p" | head -1
}

# ---- the port table --------------------------------------------------------

out="$(run -j)"

check 'port: host TX comes from tx_redirect_pkts, not a neighbouring field' \
	250 "$(port_field "$out" 2 tx_host_pkts)"
check 'port: host RX comes from rx_fw_pkts, not a neighbouring field' \
	500 "$(port_field "$out" 2 rx_host_pkts)"
check 'port: wire totals come from the netdev MIB' \
	1000 "$(port_field "$out" 2 tx_total)"
check 'port: offloaded TX is wire minus host' \
	750 "$(port_field "$out" 2 tx_offloaded)"
check 'port: offload share is a share, not a hardcoded 100' \
	75.0 "$(port_field "$out" 2 tx_offload_pct)"
check 'port: a port the firmware never punted for reads 100%' \
	100.0 "$(port_field "$out" 3 tx_offload_pct)"
check 'port: an unarmed port (netdev=-) is left out' \
	'' "$(port_field "$out" 6 tx_total)"

# The whole reason the columns are read by name. Insert a field ahead of the
# counters, exactly as the injectable= field was, and the values must not move.
{ sed 's/started=1/started=1 lookahead=9/' "$glue/status"; } > "$glue/status.new"
mv "$glue/status.new" "$glue/status"
shifted="$(run -j)"
check 'port: a field inserted before the counters does not shift them' \
	250 "$(port_field "$shifted" 2 tx_host_pkts)"
{ port_line 2 wan 1 250 500; port_line 3 lan1 1 0 0
  port_line 6 - 0 0 0; } > "$glue/status"

# ---- state -----------------------------------------------------------------

# An armed firmware whose N2H counter is not moving is the shape of a wedge,
# and it must not report as active - that reading is the whole point of taking
# two samples rather than one.
check 'state: armed firmware with a frozen counter is stalled, not active' \
	'"stalled"' "$(json_num "$out" state)"
check 'heartbeat: a frozen counter is zero pps' \
	0 "$(json_num "$out" heartbeat_pps)"

# Move the counter inside the sampling window, well clear of either end.
( sleep 0.3; printf '\tn2h_rx_pkts = 6000\n' > "$drv/n2h" ) &
moving="$(run -j)"; wait
check 'state: armed firmware with a moving counter is active' \
	'"active"' "$(json_num "$moving" state)"
check 'heartbeat: reported as the per-second delta' \
	1000 "$(json_num "$moving" heartbeat_pps)"
printf '\tn2h_rx_pkts = 5000\n' > "$drv/n2h"

echo 0x0 > "$glue/fw_mask"
check 'state: no armed port is host mode' \
	'"host"' "$(json_num "$(run -j)" state)"
echo 0x3c > "$glue/fw_mask"

# ---- the shaper's default leaf ---------------------------------------------
# An NSS tree with no default leaf drops every packet handed to it while tc
# still lists a shaper and the carrier stays set. The report has to name it.

printf "sqm.wan=queue\nsqm.wan.enabled='1'\nsqm.wan.interface='pppoe-wan'\n" > "$root/uci.out"

check 'sqm: the shaped device is resolved by section type, not by name' \
	'"pppoe-wan"' "$(json_num "$(run -j)" device)"

if [ "$stubs_honoured" = 0 ]; then
	skip 'sqm: the default-leaf cases' \
		"this shell runs a built-in tc, so the stub never gets asked"
else
	printf 'qdisc nsstbl 1: root refcnt 2 rate 165Mbit\n' > "$root/tc.out"
	check 'sqm: a root with no default leaf is reported as dropping' \
		1 "$(run | grep -c 'dropping')"

	printf 'qdisc nsstbl 1: root refcnt 2 rate 165Mbit set_default\n' > "$root/tc.out"
	check 'sqm: a root with a default leaf is not' \
		0 "$(run | grep -c 'dropping')"
fi

# ---- the L2 per-port view --------------------------------------------------
# host->fw counts frames handed to nss-drv, which reports a silent drop as a
# successful transmit, so the drop counter beside it is the only thing that
# separates "the firmware took these" from "the firmware refused every one".
# Two field captures of a dead WAN were unreadable for want of exactly this.

stub devmem 'echo 0x05050505'
for p in 2 3; do
	mkdir -p "$drv/edma/ports/$p"
	printf '\tedma_port[%s]_rx_pkts = 111%s\n\tedma_port[%s]_tx_pkts = 222%s\n' \
		"$p" "$p" "$p" "$p" > "$drv/edma/ports/$p/stats"
done

# Resolve the column from the header rather than counting it. A positional
# read is exactly what this file exists to catch, and adding the fw_link
# column broke a first draft of these three checks that had counted.
l2_field() { # l2_field <port> <column header>
	run -l2 | awk -v want="$2" -v port="$1" '
		$1 == "port" { for (i = 1; i <= NF; i++) if ($i == want) col = i; next }
		$1 == port && col { print $col }'
}

check 'l2: hostdrop reports the drop counter, not a neighbouring field' \
	9 "$(l2_field wan hostdrop)"
check 'l2: host->fw is still the redirect counter beside it' \
	250 "$(l2_field wan 'host->fw')"
check 'l2: fw->wire is the firmware egress counter, not its ingress twin' \
	1112 "$(l2_field wan 'fw->wire')"
check 'l2: wire->fw is the ingress counter' \
	2222 "$(l2_field wan 'wire->fw')"
check 'l2: fwl reports the firmware link state we last set' \
	1 "$(l2_field wan fwl)"

# ---- mesh: configured is not the same as offloaded -------------------------
# ath11k_nss_mesh_vdev_alloc() can fail at runtime (wrong fw line, capability
# probe reject) while the iface stays configured and up on the host path.
# mesh_cfg alone used to be reported as "offloaded"; it must now agree with
# the debugfs link list ath11k_nss_mesh_vdev_alloc() only writes on success.

printf "wireless.mesh=wifi-iface\nwireless.mesh.mode='mesh'\n" > "$root/uci.out"
mkdir -p "$root/sys/module/ath11k/parameters"
echo 1 > "$root/sys/module/ath11k/parameters/nss_offload"
mkdir -p "$root/sys/kernel/debug/ath11k/ahb-c000000.wifi/dbg_infra"

check 'mesh: configured but no vdev is reported as not offloaded' \
	'0' "$(json_num "$(run -j)" offloaded)"
check 'mesh: configured but no vdev says so in the text report' \
	1 "$(run | grep -c 'mesh configured, not offloaded')"

printf 'link id 39\n' > "$root/sys/kernel/debug/ath11k/ahb-c000000.wifi/dbg_infra/links"

check 'mesh: a real vdev is reported as offloaded' \
	'1' "$(json_num "$(run -j)" offloaded)"
check 'mesh: a real vdev says so in the text report' \
	1 "$(run | grep -c 'mesh offloaded')"

exit "$fail"
