#!/usr/bin/env python3
"""Copy the DTBs appended to a boot image's Image.gz (boot header v0/v1) to a file.

Qualcomm ABL on older chips (e.g. SDM636 / DRG) picks the DTB that matches the
board from the ones appended after the gzip stream, so a v0 boot image of our
own must carry the same set.

Usage: python3 tools/extract-appended-dtbs.py backup/boot_b.img dtb/appended-dtbs.bin
"""
import struct
import sys
import zlib

boot = open(sys.argv[1], "rb").read()
if boot[:8] != b"ANDROID!":
    sys.exit("not an Android boot image")
ksize = struct.unpack_from("<I", boot, 8)[0]
page = struct.unpack_from("<I", boot, 36)[0]
hver = struct.unpack_from("<I", boot, 40)[0]
if hver > 1:
    sys.exit(f"header v{hver}: DTB is a separate section, use unpack_bootv2.py")
kernel = boot[page:page + ksize]
if kernel[:2] != b"\x1f\x8b":
    sys.exit("kernel is not gzip; appended-DTB layout not recognised")
d = zlib.decompressobj(16 + zlib.MAX_WBITS)
d.decompress(kernel)
dtbs = d.unused_data
if dtbs[:4] != b"\xd0\x0d\xfe\xed":
    sys.exit("no DTBs after the gzip stream")
open(sys.argv[2], "wb").write(dtbs)
print(f"{sys.argv[2]}: {dtbs.count(b'\xd0\x0d\xfe\xed')} DTBs, {len(dtbs)} bytes")
