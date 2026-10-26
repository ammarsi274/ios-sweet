#!/bin/bash
# Build the kernel on your own Mac, exactly as .github/workflows/xnu.yml does
# on GitHub's macos-14 runners. Run it from a checkout of this repo:
#
#   git clone -b drg-port https://github.com/ammarsi274/ios-sweet
#   cd ios-sweet && bash xnu/build-on-mac.sh            # board drg (Nokia 6.1 Plus)
#   BOARD=sweet bash xnu/build-on-mac.sh                # Redmi Note 10 Pro
#
# Needs: Xcode 15.x (Xcode 16's iig does not match xnu-8796; it may be
# installed next to a newer Xcode as /Applications/Xcode_15*.app), Homebrew,
# ~20 GB free. darwin-xnu-build downloads and installs the 13.5 KDK with sudo.
# Result: xnu/out-<board>/kernel.development.<board> + SHA256SUMS.
set -euo pipefail
cd "$(dirname "$0")/.."

BOARD=${BOARD:-drg}
case "$BOARD" in
drg) MACHINE_CONFIG=DRG ;;
sweet) MACHINE_CONFIG=SWEET ;;
*) echo "BOARD must be drg or sweet" >&2; exit 1 ;;
esac
DXB_REPO=https://github.com/blacktop/darwin-xnu-build
DXB_SHA=663906528d1cd5ea8fde7de6f16de73db9511549   # same pin as xnu.yml
WORK=${WORK:-$HOME/xnu-build-$BOARD}
OUT=$PWD/xnu/out-$BOARD

[ "$(uname)" = Darwin ] || { echo "run this on macOS" >&2; exit 1; }

# Xcode 15.x: the oldest installed Xcode_*.app, else the selected one (as CI does)
XC=$(ls -d /Applications/Xcode_15*.app 2>/dev/null | sort -V | head -1 || true)
[ -n "$XC" ] || XC=$(dirname "$(dirname "$(xcode-select -p)")")
export DEVELOPER_DIR="$XC/Contents/Developer"
xv=$(DEVELOPER_DIR=$DEVELOPER_DIR xcodebuild -version | head -1)
echo "Using $xv ($XC)"
case "$xv" in
"Xcode 15."*) ;;
*) echo "warning: $xv is not Xcode 15.x; xnu-8796's IORPC code may not build" >&2 ;;
esac

command -v brew >/dev/null || { echo "install Homebrew first: https://brew.sh" >&2; exit 1; }
brew list jq gum bash cmake ninja >/dev/null 2>&1 || brew install jq gum bash cmake ninja
# build.sh only checks that xcodes exists (Xcode is already installed)
command -v xcodes >/dev/null || { mkdir -p "$WORK/bin"; printf '#!/bin/sh\nexit 0\n' > "$WORK/bin/xcodes"; chmod +x "$WORK/bin/xcodes"; export PATH="$WORK/bin:$PATH"; }

rm -rf "$WORK/dxb"
mkdir -p "$WORK"
git init -q "$WORK/dxb"
git -C "$WORK/dxb" fetch -q --depth 1 "$DXB_REPO" "$DXB_SHA"
git -C "$WORK/dxb" checkout -q FETCH_HEAD
cp xnu/patches/*.patch xnu/patches/*.sh "$WORK/dxb/patches/"
if [ "$BOARD" = drg ]; then python3 xnu/drg-overlay.py "$WORK/dxb/patches"; fi
# libfirehose_kernel is built arm64e-only; our kernels are plain arm64
sed -i '' 's/ARCHS="x86_64 arm64e" VALID_ARCHS="x86_64 arm64e"/ARCHS="x86_64 arm64e arm64" VALID_ARCHS="x86_64 arm64e arm64"/' "$WORK/dxb/build.sh"
grep -q 'VALID_ARCHS="x86_64 arm64e arm64"' "$WORK/dxb/build.sh"

cd "$WORK/dxb"
MACOS_VERSION=13.5 KERNEL_CONFIG=DEVELOPMENT MACHINE_CONFIG=$MACHINE_CONFIG MAKEFLAGS=-k \
	bash build.sh 2>&1 | tee "$WORK/build.log"
# dxb only warns when a patch does not apply; one of ours missing = broken kernel
if grep -E 'Skipping patch .*/[0-9]{4}-' "$WORK/build.log"; then
	echo "error: a patch of the series did not apply (see above)" >&2
	exit 1
fi

mkdir -p "$OUT"
cp -R fakeroot/System/Library/Kernels/. "$OUT/"
(cd "$OUT" && shasum -a 256 kernel.* > SHA256SUMS && ls -la && cat SHA256SUMS)
echo "Done. Copy $OUT/kernel.development.$BOARD to the PC: ios-sweet/xnu/out-$BOARD/"
