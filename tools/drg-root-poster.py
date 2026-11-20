#!/usr/bin/env python3
"""Load libdrgposter.dylib into PosterBoard (research/reports/wallpaper.md; root r24).

  drg-root-poster.py MOUNTED_ROOT LIBDRGPOSTER
  drg-root-poster.py --check FILE [LIB]  verify an executable's weak load command of LIB (default
                                        libdrgposter's path) and its signature's page hashes
  drg-root-poster.py --weak FILE LIB      add an LC_LOAD_WEAK_DYLIB of LIB (an install path on the phone)
                                        to any ad hoc signed executable FILE, as below (root r25:
                                        libdrgeagl into /usr/libexec/backboardd, drg-root-gpudraw.py)

LIBDRGPOSTER  tools/ios/build-drgposter.sh's libdrgposter.dylib

Adds or replaces /usr/local/lib/libdrgposter.dylib and loads it into PosterBoard two ways:

1. EnvironmentVariables.DYLD_INSERT_LIBRARIES on the LaunchAngel com.apple.PosterBoard (launchd's cache
   and the job's own plist), as tools/drg-root-gpudraw.py does for backboardd. FACT (boot #53,
   /private/var/log/com.apple.xpc.launchd/launchd.log): launchd drops it for Apple jobs, "(com.apple.
   backboardd) <Error>: Disallowing environment variable: DYLD_INSERT_LIBRARIES" (iOS 16 launchd
   0x10001a720: DYLD_* other than a DYLD_PRINT_* list is refused unless job flag bit 39 is set; the
   set sites need a label outside com.apple. or a caller check). Kept: harmless, and launchd's log
   then shows it again.
2. What does load it: an LC_LOAD_WEAK_DYLIB of the library in /Applications/PosterBoard.app/PosterBoard,
   placed after the executable's last dylib command (existing dylib ordinals unchanged; weak, so a
   missing library does not stop PosterBoard), in the header's padding (the file size, segments and
   the signature's place stay). The executable is ad hoc signed (one SHA-256 CodeDirectory, flags
   0x2, empty CMS); the hashes of the header pages are recomputed in it, nothing else changes (the
   entitlement blobs and their special-slot hashes stay as Apple's). DRG's AMFI (drg_amfi.c) treats
   every validly signed binary on the root filesystem as a platform binary and reads the XML
   entitlements from the signature; there is no trust cache, so the new cdhash is not checked
   against one. dyld's prebuilt launch loader for PosterBoard no longer matches the changed file,
   so dyld builds the loaders at launch and sees the new command.
Idempotent. Prints what it did and verifies every page hash of the result.
"""
import hashlib, os, plistlib, shutil, struct, sys

LIB = '/usr/local/lib/libdrgposter.dylib'
ANGEL = '/System/Library/LaunchAngels/com.apple.PosterBoard.plist'
EXE = 'Applications/PosterBoard.app/PosterBoard'
LC_LOAD_DYLIB, LC_LOAD_WEAK_DYLIB, LC_REEXPORT_DYLIB, LC_LAZY_LOAD_DYLIB, LC_LOAD_UPWARD_DYLIB = \
    0xc, 0x80000018, 0x8000001f, 0x20, 0x80000023
DYLIB_CMDS = (LC_LOAD_DYLIB, LC_LOAD_WEAK_DYLIB, LC_REEXPORT_DYLIB, LC_LAZY_LOAD_DYLIB, LC_LOAD_UPWARD_DYLIB)
LC_CODE_SIGNATURE, LC_SEGMENT_64 = 0x1d, 0x19


def load(path):
    with open(path, 'rb') as f:
        return plistlib.load(f)


def save(path, obj):
    st = os.stat(path)
    with open(path, 'rb') as f:
        xml = f.read(6) == b'<?xml '
    with open(path, 'wb') as f:	# in the format it had
        plistlib.dump(obj, f, fmt=plistlib.FMT_XML if xml else plistlib.FMT_BINARY)
    os.chown(path, st.st_uid, st.st_gid)
    os.chmod(path, st.st_mode & 0o7777)


def commands(d):
    magic, _, _, _, ncmds, sizeofcmds, _, _ = struct.unpack_from('<IiiIIIII', d, 0)
    if magic != 0xfeedfacf:
        raise SystemExit('error: not a 64-bit Mach-O')
    off, out = 32, []
    for _ in range(ncmds):
        c, cs = struct.unpack_from('<II', d, off)
        out.append((c, off, cs))
        off += cs
    if off != 32 + sizeofcmds:
        raise SystemExit('error: load commands do not add up')
    return out


def dylib_name(d, off):
    name_off = struct.unpack_from('<I', d, off + 8)[0]
    cs = struct.unpack_from('<I', d, off + 4)[0]
    return d[off + name_off:off + cs].split(b'\0')[0].decode()


def first_section_offset(d, cmds):
    low = None
    for c, off, _ in cmds:
        if c != LC_SEGMENT_64:
            continue
        nsects = struct.unpack_from('<I', d, off + 64)[0]
        for k in range(nsects):
            s = off + 72 + 80 * k
            o = struct.unpack_from('<I', d, s + 48)[0]
            size = struct.unpack_from('<Q', d, s + 40)[0]
            flags = struct.unpack_from('<I', d, s + 64)[0]
            if o and size and (flags & 0xff) not in (0x1, 0xc, 0x12):	# not zerofill
                low = o if low is None else min(low, o)
    return low


def signature(d, cmds):
    cs = [x for x in cmds if x[0] == LC_CODE_SIGNATURE]
    if len(cs) != 1:
        raise SystemExit('error: no single LC_CODE_SIGNATURE')
    dataoff, datasize = struct.unpack_from('<II', d, cs[0][1] + 8)
    m, _, n = struct.unpack_from('>III', d, dataoff)
    if m != 0xfade0cc0:
        raise SystemExit('error: no embedded signature super blob')
    blobs = []
    for i in range(n):
        t, o = struct.unpack_from('>II', d, dataoff + 12 + 8 * i)
        bm, bl = struct.unpack_from('>II', d, dataoff + o)
        blobs.append((t, dataoff + o, bm, bl))
    return dataoff, datasize, blobs


def code_directories(d, blobs):
    out = []
    for t, o, bm, bl in blobs:
        if bm != 0xfade0c02:
            continue
        ver, flags, hashoff, identoff, nspecial, ncode, limit, hsize, htype, _, pgs = \
            struct.unpack_from('>IIIIIIIBBBB', d, o + 8)
        if htype not in (1, 2):
            raise SystemExit('error: CodeDirectory hash type %d' % htype)
        out.append(dict(off=o, ver=ver, flags=flags, hashoff=o + hashoff, nspecial=nspecial, ncode=ncode,
                        limit=limit, hsize=hsize, H=hashlib.sha256 if htype == 2 else hashlib.sha1,
                        page=1 << pgs if pgs else 0))
    if not out:
        raise SystemExit('error: no CodeDirectory')
    return out


def page_hashes_bad(d, cd):
    bad = []
    for k in range(cd['ncode']):
        pg = d[k * cd['page']:min((k + 1) * cd['page'], cd['limit'])]
        h = cd['H'](pg).digest()[:cd['hsize']]
        if d[cd['hashoff'] + k * cd['hsize']:cd['hashoff'] + (k + 1) * cd['hsize']] != h:
            bad.append(k)
    return bad


def patch_exe(path, lib=LIB):
    with open(path, 'rb') as f:
        d = bytearray(f.read())
    cmds = commands(d)
    names = [dylib_name(d, off) for c, off, _ in cmds if c in DYLIB_CMDS]
    if lib in names:
        print(f'{path}: already loads {lib}')
        return check(path, lib)
    dataoff, datasize, blobs = signature(d, cmds)
    cds = code_directories(d, blobs)
    for cd in cds:
        if page_hashes_bad(d, cd):
            raise SystemExit('error: the original signature does not verify; not touching it')
        if cd['limit'] != dataoff or cd['ncode'] != (cd['limit'] + cd['page'] - 1) // cd['page']:
            raise SystemExit('error: unexpected code limit')
    before = {t: bytes(d[o:o + bl]) for t, o, bm, bl in blobs if bm != 0xfade0c02}

    path_b = lib.encode() + b'\0'
    size = (24 + len(path_b) + 7) & ~7
    # timestamp 2 as ld writes it; current/compatibility version 0, as the library's own LC_ID_DYLIB
    # (0.0.0), so no version check can refuse it
    new = struct.pack('<IIIIII', LC_LOAD_WEAK_DYLIB, size, 24, 2, 0, 0) + path_b
    new += b'\0' * (size - len(new))
    end = 32 + struct.unpack_from('<I', d, 20)[0]
    low = first_section_offset(d, cmds)
    if end + size > low:
        raise SystemExit(f'error: no room for the load command ({end} + {size} > {low})')
    if any(d[end:end + size]):
        raise SystemExit('error: the header padding is not empty')
    last_dylib = max(i for i, (c, _, _) in enumerate(cmds) if c in DYLIB_CMDS)
    at = cmds[last_dylib][1] + cmds[last_dylib][2]
    d[at:end + size] = new + d[at:end]	# shift the commands after it by `size`
    ncmds, sizeofcmds = struct.unpack_from('<II', d, 16)
    struct.pack_into('<II', d, 16, ncmds + 1, sizeofcmds + size)
    assert len(d) == len(bytes(d)) and commands(d)

    for cd in cds:	# rehash every page whose bytes changed (here: the header's)
        for k in range(cd['ncode']):
            pg = d[k * cd['page']:min((k + 1) * cd['page'], cd['limit'])]
            h = cd['H'](pg).digest()[:cd['hsize']]
            d[cd['hashoff'] + k * cd['hsize']:cd['hashoff'] + (k + 1) * cd['hsize']] = h
    for t, o, bm, bl in blobs:
        if bm != 0xfade0c02 and bytes(d[o:o + bl]) != before[t]:
            raise SystemExit('error: a non-CodeDirectory blob changed')
    st = os.stat(path)
    with open(path, 'r+b') as f:	# in place: same inode, owner and mode
        f.write(d)
    os.utime(path, None)
    print(f'{path}: LC_LOAD_WEAK_DYLIB {lib} added after load command {last_dylib} '
          f'(ncmds {ncmds} -> {ncmds + 1}, sizeofcmds {sizeofcmds} -> {sizeofcmds + size}, first section at {low}); '
          f'mode {oct(st.st_mode & 0o7777)}')
    return check(path, lib)


def check(path, lib=LIB):
    with open(path, 'rb') as f:
        d = f.read()
    cmds = commands(d)
    names = [(c, dylib_name(d, off)) for c, off, _ in cmds if c in DYLIB_CMDS]
    weak = [n for c, n in names if c == LC_LOAD_WEAK_DYLIB and n == lib]
    dataoff, datasize, blobs = signature(d, cmds)
    cds = code_directories(d, blobs)
    ok = len(weak) == 1 and cmds[-1][0] == LC_CODE_SIGNATURE
    for cd in cds:
        bad = page_hashes_bad(d, cd)
        ok &= not bad and cd['limit'] == dataoff
        cdhash = cd['H'](d[cd['off']:cd['off'] + struct.unpack_from('>I', d, cd['off'] + 4)[0]]).hexdigest()[:40]
        print(f'  CodeDirectory v{cd["ver"]:#x} flags {cd["flags"]:#x}: {cd["ncode"]} pages, bad page hashes {bad}, '
              f'cdhash {cdhash}')
    ents = [bm for _, _, bm, _ in blobs]
    print(f'  {len(names)} dylib commands, ours #{[n for _, n in names].index(lib) + 1 if weak else "-"} (weak); '
          f'blobs {[hex(b) for b in ents]}; last command {cmds[-1][0]:#x}')
    if not ok:
        raise SystemExit(f'error: {path} does not verify')
    print(f'  {path}: OK')
    return True


def main():
    if len(sys.argv) in (3, 4) and sys.argv[1] == '--check':
        check(*sys.argv[2:])
        return
    if len(sys.argv) == 4 and sys.argv[1] == '--weak':
        patch_exe(sys.argv[2], sys.argv[3])
        return
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    root, lib = sys.argv[1:]
    dst = os.path.join(root, LIB.lstrip('/'))
    os.makedirs(os.path.dirname(dst), mode=0o755, exist_ok=True)
    shutil.copyfile(lib, dst)
    os.chown(dst, 0, 0)
    os.chmod(dst, 0o755)
    print(f'{LIB} installed')

    path = os.path.join(root, 'System/Library/xpc/launchd.plist')
    plist = load(path)
    job = plist['LaunchDaemons'][ANGEL]
    job.setdefault('EnvironmentVariables', {})['DYLD_INSERT_LIBRARIES'] = LIB
    save(path, plist)
    own = os.path.join(root, ANGEL.lstrip('/'))
    j2 = load(own)
    j2.setdefault('EnvironmentVariables', {})['DYLD_INSERT_LIBRARIES'] = LIB
    save(own, j2)
    print(f'{ANGEL}: EnvironmentVariables {job["EnvironmentVariables"]} (launchd drops it for Apple jobs, see above)')

    patch_exe(os.path.join(root, EXE))


if __name__ == '__main__':
    main()
