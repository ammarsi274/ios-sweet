#!/bin/bash
# Stand-in for Xcode's iig (xnu's makedefs take IIG from the environment).
# Xcode 16+ iig emits IORPC.kernelContent, which xnu-8796 lacks, and the
# runners that still had Xcode 15 are retired. iig's output only depends on
# iokit/DriverKit/*.iig, which no patch touches, so the files Xcode 15.0.1's
# iig (DriverKit-286) generated in run 40579543439 are kept in
# xnu/iig-xcode15 and copied out here. A changed .iig input fails the build.
# Called as: iig --def X.iig --header OUT/X.h --impl OUT/X.iig.cpp ... -- CFLAGS
set -euo pipefail
PINNED=$(cd "$(dirname "$0")/../xnu/iig-xcode15" && pwd)
def= header= impl=
while [ $# -gt 0 ]; do
	case "$1" in
	--def) def=$2; shift 2 ;;
	--header) header=$2; shift 2 ;;
	--impl) impl=$2; shift 2 ;;
	--) break ;;
	*) shift ;;
	esac
done
if [ -z "$def" ] || [ -z "$header" ] || [ -z "$impl" ]; then
	echo "iig-pinned: need --def, --header and --impl" >&2
	exit 1
fi
name=$(basename "$def" .iig)
want=$(awk -v f="$name.iig" '$2 == f { print $1 }' "$PINNED/inputs.sha256")
have=$(shasum -a 256 "$def" | cut -d' ' -f1)
if [ -z "$want" ] || [ "$want" != "$have" ]; then
	echo "iig-pinned: $def is not the xnu-8796.141.3 input the pinned output was generated from" >&2
	exit 1
fi
cp "$PINNED/$name.h" "$header"
cp "$PINNED/$name.iig.cpp" "$impl"
