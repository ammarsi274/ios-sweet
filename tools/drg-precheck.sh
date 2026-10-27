#!/bin/bash
# Check the DRG kernel patch series on this PC before spending a CI run:
#   1. apply the whole series the way dxb does (tools/test-apply-series.sh,
#      fails on any patch that does not apply)
#   2. copy every file the DRG-only patches (xnu/patches-drg) touch into a
#      CI build tree (xnu-buildtree artifact of an earlier run)
#   3. compile them with CI's own flags (tools/xnu-cc.py, clang, syntax only)
# Usage (WSL, as root): tools/drg-precheck.sh [CI_LOG] [BUILDTREE]
#   defaults: /mnt/f/Nokia-HackiOs/ci-cc/run.log and /root/drgcc/bt
# Needs /root/drgxnu/xnu-src (xnu-8796.141.3) and /root/drgxnu/dxb.
set -euo pipefail
REPO=$(cd "$(dirname "$0")/.." && pwd)
LOG=${1:-/mnt/f/Nokia-HackiOs/ci-cc/run.log}
BT=${2:-/root/drgcc/bt}
WORK=/root/drgxnu/precheck
rm -rf "$WORK"
mkdir -p "$WORK"

cp -a "$REPO/xnu/patches" "$WORK/series"
python3 "$REPO/xnu/drg-overlay.py" "$WORK/series" | tail -1
KEEP_TREE="$WORK/tree" bash "$REPO/tools/test-apply-series.sh" \
	/root/drgxnu/xnu-src /root/drgxnu/dxb "$WORK/series" DRG | tail -1

FILES=$(sed -n 's|^+++ b/||p' "$REPO"/xnu/patches-drg/*.patch | sort -u)
SRCS=()
for f in $FILES; do
	mkdir -p "$BT/xnu/$(dirname "$f")"
	cp "$WORK/tree/$f" "$BT/xnu/$f"
	case "$f" in *.c|*.cpp|*.s|*.S) SRCS+=("$f") ;; esac
done
echo "compile-checking ${#SRCS[@]} files: ${SRCS[*]}"
XNU_CC_DXB=/Users/runner/work/_temp/xnu-build/dxb XNU_CC_CLANG=/usr/bin/clang \
	python3 "$REPO/tools/xnu-cc.py" "$LOG" "$BT" --per-file "${SRCS[@]}"
