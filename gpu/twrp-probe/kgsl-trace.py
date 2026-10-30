#!/usr/bin/env python3
"""Decode a gles-probe trace (KGSL ioctls/mmaps of Nokia's Adreno driver).

  kgsl-trace.py TRACE.bin MSM_KGSL_H [--bytes]

Ioctl names come from HMD's DRG kernel header include/uapi/linux/msm_kgsl.h
(IOCTL_KGSL_* = _IOxx(KGSL_IOC_TYPE 0x09, nr, struct)). Record format: see
gles-probe.c.
"""
import re, struct, sys
from collections import Counter

HDR = struct.Struct('<IIiIQQqiI')
KIND = {1: 'ioctl', 2: 'mmap', 3: 'munmap', 4: 'open'}


def names(header):
    src = open(header).read()
    out = {}
    for m in re.finditer(r'#define\s+(IOCTL_KGSL_\w+)\s*\\?\s*_IO(?:WR|W|R)?\(\s*KGSL_IOC_TYPE\s*,\s*(0x[0-9a-fA-F]+|\d+)', src):
        out.setdefault(int(m.group(2), 0), []).append(m.group(1)[len('IOCTL_KGSL_'):])
    return out


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    data = open(sys.argv[1], 'rb').read()
    nm = names(sys.argv[2])
    dump = '--bytes' in sys.argv
    pos, n, stats = 0, 0, Counter()
    while pos + HDR.size <= len(data):
        magic, kind, fd, cmd, a, b, ret, err, ln = HDR.unpack_from(data, pos)
        if magic != 0x4354524b:
            sys.exit(f'bad magic at {pos:#x}')
        pos += HDR.size
        if kind in (5, 6):
            body = data[pos:pos + ln]
            pos += ln
            n += 1
            stats[f'kind {kind} bytes'] += ln
            if kind == 5:
                nr, tag = cmd >> 16, cmd & 0xffff
                what = f'property {tag:#x}' if nr == 2 else ['', 'cmdlist', 'objlist', 'synclist'][tag] if tag < 4 else f'IB {tag - 4}'
                print(f'{n:4}   buffer {what} @ {a:#x} len {ln:#x}')
                if dump or nr == 2 or tag < 4:
                    for i in range(0, min(ln, 0x400 if not dump else ln), 16):
                        print(f'       +{i:03x}     {body[i:i + 16].hex(" ")}')
            elif dump:
                print(f'{n:4}   snapshot @ {a:#x} len {ln:#x}')
            continue
        before, after = data[pos:pos + ln], data[pos + ln:pos + 2 * ln]
        pos += 2 * ln
        n += 1
        if kind == 1:
            typ, nr, size, d = (cmd >> 8) & 0xff, cmd & 0xff, (cmd >> 16) & 0x3fff, cmd >> 30
            if typ == 0x09:
                name = '/'.join(nm.get(nr, ['?'])) + f' (nr {nr:#x})'
            elif typ == 0x3e:
                name = f'SYNC nr {nr}'
            else:
                name = f'type {typ:#x} nr {nr:#x}'
            stats[name] += 1
            print(f'{n:4} ioctl fd {fd} {cmd:08x} {name} size {size} dir {d} -> {ret}{" errno %d" % err if err else ""}')
            if dump and ln:
                for i in range(0, ln, 16):
                    x, y = before[i:i + 16].hex(' '), after[i:i + 16].hex(' ')
                    print(f'       +{i:03x} in  {x}\n            out {y}' if x != y else f'       +{i:03x}     {x}')
        elif kind == 2:
            prot, flags = cmd & 0xff, cmd >> 8
            stats['mmap'] += 1
            print(f'{n:4} mmap fd {fd} len {a:#x} off {b:#x} prot {prot} flags {flags:#x} -> {ret:#x}{" errno %d" % err if err else ""}')
        elif kind == 3:
            stats['munmap'] += 1
            if dump:
                print(f'{n:4} munmap {a:#x} len {b:#x} -> {ret}')
        else:
            print(f'{n:4} {KIND.get(kind, kind)} fd {fd} flags {cmd:#o} -> {ret}')
    print('\ncounts:')
    for k, v in stats.most_common():
        print(f'  {v:5} {k}')


if __name__ == '__main__':
    main()
