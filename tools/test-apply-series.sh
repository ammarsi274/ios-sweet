#!/bin/bash
# Apply the kernel patch series to a pristine xnu tree exactly the way
# darwin-xnu-build does (stage scripts first, then every *.patch in glob
# order via `git apply`), but FAIL on any patch that does not apply:
# dxb only warns and skips, which would silently build a broken kernel.
#
# Usage: tools/test-apply-series.sh XNU_SRC DXB_DIR SERIES_DIR MACHINE_CONFIG
#   XNU_SRC     pristine apple-oss-distributions/xnu checkout (xnu-8796.141.3)
#   DXB_DIR     darwin-xnu-build checkout (its top-level patches/ are applied too)
#   SERIES_DIR  our *.patch and *.sh (xnu/patches, or a DRG-transformed copy)
# Runs on Linux: a stub xcrun stands in for the macOS SDK (the stage scripts
# only copy TargetConditionals.h from it). KEEP_TREE=DIR keeps the patched
# xnu tree there for inspection.
set -euo pipefail
XNU_SRC=$(realpath "$1") DXB=$(realpath "$2") SERIES=$(realpath "$3") CFG=$4
WORK=$(mktemp -d)
trap 'if [ -n "${KEEP_TREE:-}" ]; then rm -rf "$KEEP_TREE"; mv "$WORK/xnu" "$KEEP_TREE"; fi; rm -rf "$WORK"' EXIT

mkdir -p "$WORK/sdk/usr/include" "$WORK/bin" "$WORK/patches"
echo '/* stub for apply tests */' > "$WORK/sdk/usr/include/TargetConditionals.h"
printf '#!/bin/sh\necho %s\n' "$WORK/sdk" > "$WORK/bin/xcrun"
chmod +x "$WORK/bin/xcrun"

cp -a "$XNU_SRC" "$WORK/xnu"
cp "$DXB"/patches/*.patch "$WORK/patches/"
cp "$SERIES"/*.patch "$SERIES"/*.sh "$WORK/patches/"

cd "$WORK/xnu"
for s in "$WORK"/patches/*.sh; do
	MACHINE_CONFIG=$CFG MACOS_VERSION=13.5 PATH="$WORK/bin:$PATH" bash "$s" >/dev/null
	echo "staged  $(basename "$s")"
done

applied=0 failed=()
for p in "$WORK"/patches/*.patch; do
	if git apply --check "$p" 2>/dev/null; then
		git apply "$p"
		applied=$((applied + 1))
	else
		failed+=("$(basename "$p")")
	fi
done
echo "applied $applied patches"
ours_failed=0
for f in "${failed[@]}"; do
	if [ -e "$SERIES/$f" ]; then
		echo "FAILED  $f"
		ours_failed=1
	else
		echo "skipped $f (darwin-xnu-build's own; dxb skips it too)"
	fi
done
[ "$ours_failed" = 0 ] && echo "OK: every patch of the series applied"
exit "$ours_failed"
