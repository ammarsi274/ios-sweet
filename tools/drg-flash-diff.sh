#!/bin/bash
# Flash a new iOS root image by writing only the 1 MiB chunks that differ from
# what the phone holds now. Run in Git Bash with the phone in TWRP (RAM boot).
#   drg-flash-diff.sh NEW_IMAGE_WSL
# NEW_IMAGE_WSL without .img plus .chunks must exist (tools/chunkhash NEW 16384). Steps:
#   1. the phone hashes its userdata in 1 MiB chunks (tools/chunkhash, ~1 min)
#   2. chunkdiff-simg.py builds a sparse image of NEW's differing chunks
#   3. reboot to the bootloader, fastboot flash userdata (no boot afterwards)
set -euo pipefail
NEW=$1
N=16384
WORK=/f/Nokia-HackiOs/logs/flash
mkdir -p $WORK
DEV=$(MSYS_NO_PATHCONV=1 adb shell 'readlink -f /dev/block/bootdevice/by-name/userdata' | tr -d '\r')
[ "$(MSYS_NO_PATHCONV=1 adb shell "grep -c $DEV /proc/mounts" | tr -d '\r')" = 0 ] || { echo "userdata is mounted"; exit 1; }
MSYS_NO_PATHCONV=1 adb push F:/Nokia-HackiOs/ios-sweet/tools/chunkhash.arm64 /tmp/chunkhash >/dev/null
MSYS_NO_PATHCONV=1 adb shell chmod 755 /tmp/chunkhash
echo "$(date +%T) hashing the phone's $DEV"
MSYS_NO_PATHCONV=1 adb shell "/tmp/chunkhash $DEV $N" | tr -d '\r' > $WORK/phone.chunks
[ "$(wc -l < $WORK/phone.chunks)" = $N ] || { echo "phone list incomplete"; exit 1; }
SIMG=${NEW%.img}-diff.simg
MSYS_NO_PATHCONV=1 wsl -d Ubuntu -u root -- python3 /mnt/f/Nokia-HackiOs/ios-sweet/tools/chunkdiff-simg.py \
	$NEW ${NEW%.img}.chunks /mnt/f/Nokia-HackiOs/logs/flash/phone.chunks /mnt/f/Nokia-HackiOs/logs/flash/new-diff.simg
echo "$(date +%T) flashing"
MSYS_NO_PATHCONV=1 adb reboot bootloader
for i in $(seq 1 60); do timeout 5 fastboot getvar unlocked 2>&1 | grep -q "unlocked: yes" && break; sleep 3; done
fastboot flash userdata F:/Nokia-HackiOs/logs/flash/new-diff.simg 2>&1 | tail -3
fastboot reboot bootloader 2>&1 | tail -1
echo "$(date +%T) flashed; the phone is restarting into the bootloader"
