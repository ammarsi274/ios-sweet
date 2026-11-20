#!/bin/bash
# The iOS self-test's render-target frames on TWRP, with Nokia's own KGSL (boot #55/#56: iOS got exactly
# 1,324,512 of 2,740,500 texels right). Run from Git Bash on Windows with the phone in a TWRP RAM boot
# (adb shell as root). Pushes the bench + Nokia's driver and gralloc into TWRP's RAM (lost at reboot),
# runs it, pulls the log. Changes nothing on the phone's storage.
#   tools/drg-twrp-rt-repro.sh [BENCH]      default BENCH: F:/Nokia-HackiOs/logs/drgles-bench-rt
# The run (bench gpu/drgles, -owngbuf W H 5 -gbufcraft = the iOS buffer: BGRA, our handle with rows = h,
# our gralloc module, no metadata fd, CPU address 0; -gbufframes 60 = stress_test's 60 frames of clear +
# pattern, glFlush each, glFinish), then the same report as iOS ("gbuf: frames map" lines) and one
# "submit:" line per GPU_COMMAND of those frames.
# Read: "gbuf: render-target frames as on iOS: OK" (Nokia's driver right: the iOS bug is ours) or WRONG
# with the map (the same 1,324,512 = the blob's; anything else = compare the maps).
set -euo pipefail
BENCH=${1:-F:/Nokia-HackiOs/logs/drgles-bench-rt}
V=F:/Nokia-HackiOs/research/vendor-adreno
LOG=F:/Nokia-HackiOs/logs/twrp-rt-frames-$(date +%Y%m%d-%H%M%S).log
export MSYS_NO_PATHCONV=1
adb get-state | grep -qx device || { echo "no adb device (TWRP RAM boot expected)" >&2; exit 1; }
adb shell 'id -u' | grep -qx 0 || { echo "adb shell is not root" >&2; exit 1; }
adb shell 'mkdir -p /tmp/adreno /vendor/lib64/hw /vendor/firmware'
for f in "$V"/files/lib64/*.so "$V"/files/lib64/egl/*.so; do adb push "$f" /tmp/adreno/ >/dev/null; done
for f in "$V"/files/firmware/*; do	# as HANDOFF-PROMPT.md: all of it to /vendor/firmware
	adb push "$f" /vendor/firmware/ >/dev/null
done
adb push "$V/files/lib64/hw/gralloc.sdm660.so" /vendor/lib64/hw/gralloc.qcom.so >/dev/null
for f in "$V/files/lib64/libgrallocutils.so" "$V/files/lib64/libqdMetaData.so" "$V/probe-out/libsync.so" \
	"$V/probe-out/android.hardware.graphics.common@1.1.so"; do
	adb push "$f" /vendor/lib64/ >/dev/null
done
adb push "$BENCH" /tmp/drgles-bench >/dev/null
adb shell 'chmod 755 /tmp/drgles-bench'
adb shell 'sha256sum /tmp/drgles-bench'
adb shell 'cd /tmp && ./drgles-bench -owngbuf 1125 2436 5 -gbufcraft -gbufframes 60 /tmp/adreno > /tmp/rt.log 2>&1; echo "exit $?" >> /tmp/rt.log'
adb pull /tmp/rt.log "$LOG" >/dev/null
grep -E 'gbuf: (frames|render-target|EGLImage render target linear)|submits:|RESULT' "$LOG" | grep -v 'frames map:   ' || true
echo "log: $LOG"
