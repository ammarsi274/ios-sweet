#!/usr/bin/env python3
"""Turn the SWEET patch series into the DRG (Nokia 6.1 Plus) kernel.

dxb runs patches/*.sh before it applies patches/*.patch, so a stage script
cannot edit files the series creates. Instead this rewrites the copied
.patch files in place. Every substitution swaps one added ("+") line for
another, so hunk line counts stay valid, and each must match exactly once:
if the series changes underneath, the build stops here instead of
silently building a sweet kernel.

Usage (CI, after copying the series): python3 xnu/drg-overlay.py dxb/patches
"""
import sys
from pathlib import Path

# (patch file prefix, old added line, new added line)
SUBSTITUTIONS = [
    # --- Nokia identity: match the names tools/mkadt.py --board drg emits ---
    ("0016-", "+    \"     'IONameMatch'     = 'xiaomi,sweet';\"",
              "+    \"     'IONameMatch'     = 'nokia,drg';\""),
    ("0018-", "+    \"     'IONameMatch'     = 'qcom,kryo470';\"",
              "+    \"     'IONameMatch'     = 'qcom,kryo260';\""),
    ("0046-", "+    \"     'IONameMatch'     = 'sweet,display';\"",
              "+    \"     'IONameMatch'     = 'nokia,drg-display';\""),
    ("0003-", "+\tif (SecureDTFindEntry(\"name\", \"sweet-fbcon\", &entry) != kSuccess ||",
              "+\tif (SecureDTFindEntry(\"name\", \"drg-fbcon\", &entry) != kSuccess ||"),
]


def main(patch_dir):
    patches = sorted(Path(patch_dir).glob("*.patch"))
    for prefix, old, new in SUBSTITUTIONS:
        matches = [p for p in patches if p.name.startswith(prefix)]
        if len(matches) != 1:
            sys.exit(f"drg-overlay: expected one {prefix}*.patch, found {len(matches)}")
        path = matches[0]
        # bytes in, bytes out: never let the platform translate line endings
        lines = path.read_bytes().split(b"\n")
        hits = [i for i, line in enumerate(lines) if line == old.encode()]
        if len(hits) != 1:
            sys.exit(f"drg-overlay: {path.name}: {len(hits)} matches for {old!r}")
        lines[hits[0]] = new.encode()
        path.write_bytes(b"\n".join(lines))
        print(f"drg-overlay: {path.name}: {old.strip('+').strip()} -> {new.strip('+').strip()}")


if __name__ == "__main__":
    main(sys.argv[1])
