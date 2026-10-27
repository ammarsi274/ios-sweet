#!/usr/bin/env python3
"""Turn the sweet kernel patch series into the DRG (Nokia 6.1 Plus) series.

dxb runs patches/*.sh before it applies patches/*.patch, so a stage script
cannot edit files the series creates. Instead this rewrites the copied
series (our NNNN-*.patch and NNNN-*.sh; darwin-xnu-build's own patches are
left alone) in three passes:

  1. DRG changes: exact substrings, each with the number of times it must
     occur across the whole series. A later patch often carries an earlier
     patch's line as context, so both must change together or the later
     patch stops applying, and dxb only warns and skips a patch that does
     not apply. A count mismatch stops the build here instead.
  2. Rename: sweet/Sweet/SWEET -> drg/Drg/DRG in contents and file names
     (identifiers, file paths, the machine config, boot-args, log tags).
  3. Check: no sweet/Xiaomi/Redmi/SM7150 text may be left.
  4. Add xnu/patches-drg/*.patch (DRG-only changes to Apple's own code).

Line counts never change, so every hunk header stays valid. Prove the
result with tools/test-apply-series.sh before spending a CI run.

With --dxb DXB_DIR it also adjusts darwin-xnu-build's build.sh: the
kernel links libfirehose_kernel.a, which build.sh compiles separately
with xcodebuild and no CPU flag, i.e. for clang's apple-m1 default (LSE).
DRG builds it for arm64 only with -mcpu=cortex-a73. Run this after the
CI step that adds arm64 to that xcodebuild line.

Usage: python3 xnu/drg-overlay.py DIR [--dxb DXB_DIR]   (DIR = copy of xnu/patches; edited in place)
"""
import re
import sys
from pathlib import Path

OURS = re.compile(r"^\d{4}-.*\.(patch|sh)$")

# (old, new, occurrences in the whole series)
CHANGES = [
    # --- identity: the names tools/mkadt.py --board drg emits ---
    ("'IONameMatch'     = 'xiaomi,sweet';", "'IONameMatch'     = 'nokia,drg';", 2),
    ("'IONameMatch'     = 'qcom,kryo470';", "'IONameMatch'     = 'qcom,kryo260';", 2),       # 0018 +, 0038 context
    ("'IONameMatch'     = 'sweet,display';", "'IONameMatch'     = 'nokia,drg-display';", 2),  # 0046 +, 0058 context
    ('strlcpy(name, "Redmi Note 10 Pro", (size_t)maxLength);',
     'strlcpy(name, "Nokia 6.1 Plus", (size_t)maxLength);', 2),

    # --- CPU: Kryo 260 is ARMv8.0 (no LSE atomics, no PAN; measured on the phone) ---
    # -march alone is not enough: for macOS targets clang keeps -target-cpu
    # apple-m1, whose features (LSE, RCPC) the backend still uses. Kryo 260
    # Gold is Cortex-A73 class. tools/check-armv80.sh proves the result.
    ("-march=armv8.2-a+crypto", "-march=armv8-a+crypto -mcpu=cortex-a73", 1),
    ("+#define __ARM_PAN_AVAILABLE__     1",
     "+/* no __ARM_PAN_AVAILABLE__: Kryo 260 is ARMv8.0, ID_AA64MMFR1.PAN = 0 */", 1),
    (' * Board config for the Redmi Note 10 Pro ("sweet", Qualcomm SM7150 /',
     ' * Board config for the Nokia 6.1 Plus ("DRG", Qualcomm SDM636 /', 1),
    (" * Snapdragon 732G): Kryo 470 = Cortex-A76 + Cortex-A55, ARMv8.2, no PAC.",
     " * Snapdragon 636): Kryo 260 Gold/Silver (A73/A53 class), ARMv8.0, no PAC.", 1),

    # --- SMP: XNU runs on the Gold cluster, MPIDR 0x100-0x103, boot CPU 0x100 ---
    ("/* The supplied ADT has cpu0 as the boot CPU and ascending A55 nodes. */",
     "/* The DRG ADT has cpu0 = Gold 0x100 as the boot CPU, then 0x101-0x103. */", 1),
    ("topology->boot_cpu->phys_id != 0) {", "topology->boot_cpu->phys_id != 0x100) {", 1),
    ('expected boot MPIDR 0 and logical CPU 0"', 'expected boot MPIDR 0x100 and logical CPU 0"', 1),
    ("arg_paddr != 0 || cpu->phys_id == 0 ||", "arg_paddr != 0 || cpu->phys_id == 0x100 ||", 1),
    ("/* MPIDR Aff1 = core: 0x000-0x500 A55, 0x600/0x700 A76 */",
     "/* SDM636 Gold cluster: MPIDR Aff1 = 1, Aff0 = core 0-3 */", 1),
    ("if (cpu->phys_id > 0x700 || (cpu->phys_id & 0xff) != 0 ||",
     "if ((cpu->phys_id & ~0x3u) != 0x100 ||", 1),
    ("#define SWEET_SMP_MAX_CPUS         8", "#define SWEET_SMP_MAX_CPUS         4", 1),
    ("/* Aff1 encodes the core on SM7150, not an Apple cluster ID. */",
     "/* SDM636: Aff1 = cluster, Aff0 = core; the ADT gives cluster-core-id. */", 1),

    # --- framebuffer: splash at 0x9d400000, 1080x2280 ---
    ("0x9c000000", "0x9d400000", 6),
    ("2368 * 4320", "2248 * 4320", 1),          # early marks: 32 rows above the bottom
    ("<< 16) / 2400);", "<< 16) / 2280);", 2),
    ("fLogicalH = native ? 2400 : 2436;", "fLogicalH = native ? 2280 : 2436;", 1),
    ("fOffY = (fLogicalH - 2400) / 2;", "fOffY = (fLogicalH - 2280) / 2;", 1),

    # --- SM7150-only drivers (keys, touch + RPMh, USB): off unless asked for ---
    ("\tint enabled = 1;", "\tint enabled = 0;\t/* DRG: not this board's hardware */", 3),

    # --- persistent log header: spelled out per character, so the rename misses it ---
    ("{ 'S', 'W', 'E', 'E', 'T', 'L', 'O', 'G' }", "{ 'N', 'O', 'K', 'I', 'A', 'L', 'O', 'G' }", 1),
    ('"SWEETLOG" | u32 version 1', '"NOKIALOG" | u32 version 1', 2),    # 0047 +, 0069 context

    # --- comments that name the board ---
    ("(Redmi Note 10 Pro, SM7150)", "(Nokia 6.1 Plus, SDM636)", 3),
    ("Redmi Note 10 Pro bootstrap console", "Nokia 6.1 Plus bootstrap console", 2),
    ("boards with no usable UART (the Redmi Note", "boards with no usable UART (the Nokia 6.1", 1),
    ("10 Pro's debug UART engine runs in SPI mode)", "Plus has no reachable debug UART)", 1),
    ("for Redmi Note 10 Pro (sweet).", "for Nokia 6.1 Plus (DRG).", 1),
    ("None of that hardware exists on SM7150", "None of that hardware exists on SDM636", 1),
    ("/* GT9896 is Yellowstone in Xiaomi sweet-r-oss, not the Berlin protocol. */",
     "/* GT9896 (not on DRG, driver off): Yellowstone, not the Berlin protocol. */", 1),
]

# darwin-xnu-build build.sh (after CI added arm64): libfirehose_kernel for Kryo 260
DXB_CHANGES = [
    ('ARCHS="x86_64 arm64e arm64" VALID_ARCHS="x86_64 arm64e arm64"',
     'ARCHS="arm64" VALID_ARCHS="arm64" OTHER_CFLAGS=\'$(inherited) -mcpu=cortex-a73\'', 1),
]

RENAME = [("SWEET", "DRG"), ("Sweet", "Drg"), ("sweet", "drg")]
# also names spelled one character at a time: { 'S', 'W', 'E', 'E', 'T', ... }
LEFTOVER = re.compile(rb"sweet|xiaomi|redmi|sm7150|'S', *'W', *'E', *'E', *'T'", re.IGNORECASE)


def rename(s):
    """Works on str (file names) and bytes (contents; not all patches are UTF-8)."""
    for old, new in RENAME:
        if isinstance(s, bytes):
            old, new = old.encode(), new.encode()
        s = s.replace(old, new)
    return s


def main(patch_dir):
    files = sorted(p for p in Path(patch_dir).iterdir() if OURS.match(p.name))
    if not files:
        sys.exit(f"drg-overlay: no NNNN-* patches in {patch_dir}")
    # bytes in, bytes out: never let the platform translate line endings
    text = {p: p.read_bytes() for p in files}

    for old, new, want in CHANGES:
        o, n = old.encode(), new.encode()
        if o.count(b"\n") != n.count(b"\n"):
            sys.exit(f"drg-overlay: {old!r} would change the line count")
        got = sum(t.count(o) for t in text.values())
        if got != want:
            sys.exit(f"drg-overlay: {old!r}: {got} occurrences, expected {want}")
        text = {p: t.replace(o, n) for p, t in text.items()}

    for p in files:
        t = rename(text[p])
        p.unlink()
        out = p.with_name(rename(p.name))
        out.write_bytes(t)
        for m in LEFTOVER.finditer(t):
            line = t[:m.start()].count(b"\n") + 1
            sys.exit(f"drg-overlay: {out.name}:{line}: leftover {m.group().decode()!r}")
    # DRG-only patches (Apple code the sweet series never needed to touch,
    # e.g. hand-written LSE). Numbered 09xx so they apply after the series.
    extra = sorted((Path(__file__).parent / "patches-drg").glob("*.patch"))
    for p in extra:
        t = p.read_bytes()
        m = LEFTOVER.search(t)
        if m:
            sys.exit(f"drg-overlay: {p.name}: leftover {m.group().decode()!r}")
        (Path(patch_dir) / p.name).write_bytes(t)
    print(f"drg-overlay: {len(CHANGES)} DRG changes, {len(files)} files renamed sweet -> drg, "
          f"{len(extra)} DRG-only patches added")


def patch_dxb(dxb_dir):
    path = Path(dxb_dir) / "build.sh"
    t = path.read_bytes()
    for old, new, want in DXB_CHANGES:
        got = t.count(old.encode())
        if got != want:
            sys.exit(f"drg-overlay: build.sh: {old!r}: {got} occurrences, expected {want}")
        t = t.replace(old.encode(), new.encode())
    path.write_bytes(t)
    print(f"drg-overlay: build.sh: libfirehose_kernel for arm64 with -mcpu=cortex-a73")


if __name__ == "__main__":
    main(sys.argv[1])
    if "--dxb" in sys.argv:
        patch_dxb(sys.argv[sys.argv.index("--dxb") + 1])
