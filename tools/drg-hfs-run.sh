#!/bin/bash
# Run a shell script with an HFS+ image mounted (run in WSL as root).
#   drg-hfs-run.sh IMG rw|ro MOUNTPOINT SCRIPT [ARGS...]
# SCRIPT runs with the image mounted at MOUNTPOINT (rw: -o force, as r23 was made; ro: read-only) and the
# mount is gone afterwards; its exit status is returned.
# WSL's own kernel has no HFS+ and a module only loads into the kernel it was built for (2026-11-19: WSL
# moved from 6.18.54.1, for which /root/wslk built hfsplus.ko, to 6.18.58.1: "disagrees about version of
# symbol module_layout"). So: the host's hfsplus if it has one, else User-Mode Linux built from the same
# 6.18.54.1 source (/root/wslk/um/linux: x86_64_defconfig + HFSPLUS_FS, HOSTFS, BLK_DEV_UBD), whose root is
# the host's / through hostfs (same programs and files) and the image its ubd disk.
set -euo pipefail
IMG=$1 MODE=$2 MNT=$3 SCRIPT=$4
shift 4
[ "$MODE" = rw ] && OPT=force || OPT=ro
UML=${DRG_UML:-/root/wslk/um/linux}
mkdir -p "$MNT"
if grep -qw hfsplus /proc/filesystems || { [ -f /root/wslk/src/fs/hfsplus/hfsplus.ko ] &&
	insmod /root/wslk/src/fs/hfsplus/hfsplus.ko 2>/dev/null; }; then
	mount -t hfsplus -o loop,$OPT "$IMG" "$MNT"
	rc=0
	bash "$SCRIPT" "$@" || rc=$?
	umount "$MNT"
	exit $rc
fi
[ -x "$UML" ] || { echo "error: no hfsplus in this kernel and no User-Mode Linux at $UML" >&2; exit 1; }
W=$(mktemp -d /root/.drg-hfs-run.XXXXXX)
trap 'rm -rf "$W"' EXIT
{
	echo '#!/bin/bash'
	echo 'mount -t proc proc /proc; mount -t sysfs sys /sys; grep -q " /dev devtmpfs" /proc/mounts || mount -t devtmpfs dev /dev'
	echo 'export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin HOME=/root LANG=C.UTF-8'
	printf 'cd %q\n' "$PWD"
	printf 'rc=1; if mount -t hfsplus -o %s /dev/ubda %q; then rc=0; bash %q' "$OPT" "$MNT" "$SCRIPT"
	for a in "$@"; do printf ' %q' "$a"; done
	printf ' || rc=$?; umount %q || rc=99; fi\n' "$MNT"
	printf 'echo $rc > %q\n' "$W/status"
	echo 'sync'
	echo "python3 -c 'import ctypes; ctypes.CDLL(None).reboot(0x4321fedc)'"	# LINUX_REBOOT_CMD_POWER_OFF
} > "$W/init"
chmod +x "$W/init"
[ "$MODE" = rw ] && UBD="ubd0=$IMG" || UBD="ubd0r=$IMG"
echo "(User-Mode Linux $("$UML" --version 2>/dev/null | head -1): $MODE $IMG)"
"$UML" mem=768M "$UBD" root=/dev/root rootfstype=hostfs rootflags=/ rw init="$W/init" con=null con0=fd:0,fd:1 \
	quiet < /dev/null || true
[ -s "$W/status" ] || { echo "error: User-Mode Linux run gave no status" >&2; exit 1; }
exit "$(cat "$W/status")"
