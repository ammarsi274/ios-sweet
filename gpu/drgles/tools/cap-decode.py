#!/usr/bin/env python3
"""Summarise a drgles capture (bench -capture FILE, kgsl_bench.c).

  cap-decode.py CAPTURE.bin [--ibs]

Per submit: the command (context, timestamp, IBs with GPU address and size,
memobjs), the wait result, and which GPU objects the GPU changed (after vs.
before). --ibs prints each IB's dwords with type-4/7 packet headers decoded.
"""
import struct, sys
from collections import OrderedDict

REC = struct.Struct('<IIII')
T = {1: 'ALLOC', 2: 'MAP', 3: 'FREE', 4: 'SUBMIT', 5: 'OBJ', 6: 'PROP', 7: 'CTX', 8: 'WAIT', 9: 'MEMSTORE'}
CMD = struct.Struct('<QQIIQIIQIIII')		# kgsl_gpu_command
CMDOBJ = struct.Struct('<QQQII')		# kgsl_command_object


def records(data):
    pos = 0
    while pos + REC.size <= len(data):
        magic, typ, ln, _ = REC.unpack_from(data, pos)
        assert magic == 0x43475244, hex(pos)
        yield typ, data[pos + REC.size:pos + REC.size + ln]
        pos += REC.size + ln


def pkt(d):
    t = d >> 28
    if t == 7:
        return 'pkt7 op 0x%02x cnt %d' % ((d >> 16) & 0x7f, d & 0x3fff)
    if t == 4:
        return 'pkt4 reg 0x%05x cnt %d' % ((d >> 8) & 0x7ffff, d & 0x7f)
    return None


def owner_of(objs, addr):
    for oid, o in objs.items():
        if o['va'] is not None and o['va'] <= addr < o['va'] + o['mmapsize']:
            return oid
    return None


def scan_refs(objs, raw, base, show):
    """Print the IB; list 64-bit GPU addresses in it (lo, hi pairs with hi in the
    GPU ranges seen: 0x7 user, 0x0 global) and whether a captured object holds them."""
    words = [struct.unpack_from('<I', raw, 4 * k)[0] for k in range(len(raw) // 4)]
    refs = set()
    for k in range(len(words) - 1):
        lo, hi = words[k], words[k + 1]
        a = hi << 32 | lo
        if (hi == 7 and lo >= 0xf0000000) or (hi == 0 and 0xf8000000 <= lo < 0xfa000000):
            refs.add(a)
    for k, d in enumerate(words):
        if show:
            print('      %04x: %08x %s' % (k, d, pkt(d) or ''))
    for a in sorted(refs):
        o = owner_of(objs, a)
        print('      ref %#x -> %s' % (a, 'obj %d' % o if o is not None else 'NOT CAPTURED (global/kernel or unmapped)'))


def main():
    data = open(sys.argv[1], 'rb').read()
    want_ibs = '--ibs' in sys.argv
    objs = OrderedDict()		# id -> dict
    content = {}			# id -> latest bytes
    before = {}
    va2id = {}
    for typ, p in records(data):
        name = T.get(typ, str(typ))
        if typ == 1:
            size, flags, va_len, mmapsize, oid, mlen, meta = struct.unpack_from('<QQQQIIQ', p)
            objs[oid] = dict(size=size, flags=flags, mmapsize=mmapsize, va=None)
        elif typ == 2:
            off, va, ln, prot, fl = struct.unpack_from('<QQQII', p)
            oid = off >> 12
            if oid in objs:
                objs[oid]['va'] = va
                va2id[va] = oid
        elif typ == 3:
            oid, = struct.unpack_from('<I', p)
            objs.pop(oid, None)
        elif typ == 6:
            ptype, _, val, sz = struct.unpack_from('<IIQQ', p)
            print('PROP type %#x: %s' % (ptype, p[24:24 + sz].hex()))
        elif typ == 7:
            fl, cid = struct.unpack_from('<II', p)
            print('CTX flags %#x -> id %d' % (fl, cid))
        elif typ == 9 and len(p) == 32 + 8 and False:
            pass
        elif typ == 5:
            oid, phase, sub, has, gva, ln, h = struct.unpack_from('<IIIIQQQ', p)
            if has:
                content[oid] = p[40:40 + ln]
            if phase == 0:
                before[oid] = h
            else:
                if before.get(oid) != h:
                    o = objs.get(oid, {})
                    print('   changed by GPU: obj %d gpuaddr %#x len %#x flags %#x' % (oid, gva, ln, o.get('flags', 0)))
        elif typ == 4:
            sub, = struct.unpack_from('<I', p)
            c = CMD.unpack_from(p, 4)
            flags, cl, cs, nc, ol, os_, no, sl, ss, ns, ctx, ts = c
            print('SUBMIT %d: ctx %d ts %d flags %#x cmds %d objs %d syncs %d' % (sub, ctx, ts, flags, nc, no, ns))
            off = 4 + CMD.size
            for i in range(nc):
                o, ga, sz, fl, oid = CMDOBJ.unpack_from(p, off + i * cs)
                owner = owner_of(objs, ga)
                print('   IB %d: gpuaddr %#x size %#x flags %#x id %d offset %#x -> obj %s' % (i, ga, sz, fl, oid, o, owner))
                if owner is not None and owner in content:
                    base = objs[owner]['va']
                    raw = content[owner][ga - base:ga - base + sz]
                    scan_refs(objs, raw, ga, want_ibs)
            off += nc * cs
            for i in range(no):
                o, ga, sz, fl, oid = CMDOBJ.unpack_from(p, off + i * os_)
                print('   memobj %d: gpuaddr %#x size %#x flags %#x id %d' % (i, ga, sz, fl, oid))
        elif typ == 8:
            sub, ctx, ts, src, wrc = struct.unpack_from('<IIIii', p)
            print('   WAIT ctx %d ts %d: submit rc %d wait rc %d' % (ctx, ts, src, wrc))
    print('objects live at end: %d' % len(objs))


if __name__ == '__main__':
    main()
