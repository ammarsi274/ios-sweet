#!/bin/bash
# Live kernel log of a booting DRG phone (patch 0921), from Git Bash on Windows.
#   tools/drg-livelog.sh [LOGFILE]     default: logs/live-<time>.txt
# Waits for the phone's iOS USB function (VID 1209 PID 0001, boot-arg drgusb=2), attaches it to WSL with
# usbipd-win (no admin: AutoBind policy for 1209:0001 added 2026-11-26), gives the cdc_ether interface
# 192.168.7.1/24 and prints/saves the UDP log stream the phone sends to 192.168.7.1:5140 (the phone is
# 192.168.7.2). Stop with Ctrl-C; the device detaches when the phone reboots.
set -uo pipefail
LOG=${1:-/f/Nokia-HackiOs/logs/live-$(date +%Y%m%d-%H%M%S).txt}
U="/c/Program Files/usbipd-win/usbipd.exe"
echo "waiting for the phone's iOS USB device (1209:0001)..."
until "$U" list 2>/dev/null | grep -qi '1209:0001'; do sleep 1; done
"$U" attach --wsl --hardware-id 1209:0001 || { echo "usbipd attach failed"; exit 1; }
MSYS_NO_PATHCONV=1 wsl -u root -- bash -s "$(wslpath -a "$LOG" 2>/dev/null || echo /mnt/f/Nokia-HackiOs/logs/live.txt)" <<'W'
for i in $(seq 1 30); do
	IF=$(for d in /sys/class/net/*; do [ "$(basename "$(readlink -f $d/device/driver 2>/dev/null)")" = cdc_ether ] && basename $d; done | head -1)
	[ -n "$IF" ] && break; sleep 1
done
[ -n "${IF:-}" ] || { echo "no cdc_ether interface in WSL"; exit 1; }
ip addr replace 192.168.7.1/24 dev $IF; ip link set $IF up
echo "interface $IF up (192.168.7.1); log -> $1"
command -v nc >/dev/null || { echo "nc missing in WSL"; exit 1; }
nc -ukl 5140 | tee "$1"
W
