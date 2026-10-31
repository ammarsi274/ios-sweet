#!/usr/bin/env python3
"""Compare two drgles captures (kgsl_cap.c): a Linux run in TWRP against an iOS run.

  cap-diff.py LINUX.bin IOS.bin [--max N]

Objects are matched by allocation order (ids and addresses differ between the
two kernels). Before each submit, every live object's contents are compared
dword by dword. A 64-bit pair (or a lone high/low dword) that is a GPU
address inside a mapped object, or the memstore, is replaced by
"object k + offset" on both sides first, so only real differences remain.
Each difference is printed with the type-4/7 packet around it when the object
is one of the submit's IBs.
"""
import struct, sys

REC = struct.Struct('<IIII')
CMD = struct.Struct('<QQIIQIIQIIII')		# kgsl_gpu_command
CMDOBJ = struct.Struct('<QQQII')		# kgsl_command_object
OBJ = struct.Struct('<IIIIQQQ')		# id, phase, submit, has_data, gpuaddr, len, hash
ALLOC = struct.Struct('<QQQQIIQ')		# kgsl_gpuobj_alloc
MAP = struct.Struct('<QQQII')			# off, va, len, prot, flags
WAIT = struct.Struct('<IIIii')


def records(data):
    pos = 0
    while pos + REC.size <= len(data):
        magic, typ, ln, _ = REC.unpack_from(data, pos)
        assert magic == 0x43475244, hex(pos)
        yield typ, data[pos + REC.size:pos + REC.size + ln]
        pos += REC.size + ln


class Cap:
    def __init__(self, path):
        self.order = {}		# id -> allocation index
        self.va = {}		# id -> (va, len)
        self.memstore = None
        self.content = {}		# id -> latest bytes
        self.before = []		# per submit: {order: bytes}
        self.submits = []		# per submit: list of (gpuaddr, size, flags)
        self.waits = []
        n_alloc = 0
        for typ, p in records(open(path, 'rb').read()):
            if typ == 1:
                a = ALLOC.unpack_from(p)
                self.order[a[4]] = n_alloc
                n_alloc += 1
            elif typ == 2:
                off, va, ln, prot, fl = MAP.unpack_from(p)
                self.va[off >> 12] = (va, ln)
            elif typ == 9 and len(p) == MAP.size:
                self.memstore = MAP.unpack_from(p)[1:3]
            elif typ == 5:
                oid, phase, sub, has, ga, ln, h = OBJ.unpack_from(p)
                if has:
                    self.content[oid] = p[OBJ.size:]
                if phase == 0:
                    while len(self.before) <= sub:
                        self.before.append({})
                    if oid in self.content and oid in self.order:
                        self.before[sub][self.order[oid]] = self.content[oid]
            elif typ == 4 and len(p) > 4 + CMD.size:
                c = CMD.unpack_from(p, 4)
                ibs = []
                for i in range(c[3]):
                    o = CMDOBJ.unpack_from(p, 4 + CMD.size + i * c[2])
                    ibs.append((o[1], o[2], o[3]))
                self.submits.append(ibs)
            elif typ == 8:
                self.waits.append(WAIT.unpack_from(p))
        self.byorder = {self.order[i]: i for i in self.order}

    def where(self, addr):
        """GPU address -> ('obj', order, offset) / ('memstore', offset) / None"""
        for oid, (va, ln) in self.va.items():
            if va <= addr < va + ln and oid in self.order:
                return ('obj', self.order[oid], addr - va)
        if self.memstore and self.memstore[0] <= addr < self.memstore[0] + self.memstore[1]:
            return ('memstore', addr - self.memstore[0])
        return None

    def his(self):
        return {va >> 32 for va, _ in self.va.values()} | ({self.memstore[0] >> 32} if self.memstore else set())


def norm(cap, d):
    """dwords -> tokens with GPU addresses replaced"""
    out = list(d)
    his = cap.his()
    i = 0
    while i < len(d):
        if i + 1 < len(d) and d[i + 1] in his and d[i + 1] != 0:
            w = cap.where(d[i] | d[i + 1] << 32)
            if w:
                out[i], out[i + 1] = ('lo',) + w, ('hi',) + w[:2]
                i += 2
                continue
        i += 1
    return out


def pkt(d):
    t = d >> 28
    if t == 7:
        return 'pkt7 op 0x%02x cnt %d' % ((d >> 16) & 0x7f, d & 0x3fff)
    if t == 4:
        return 'pkt4 reg 0x%05x cnt %d' % ((d >> 8) & 0x7ffff, d & 0x7f)
    return None


def headers(d):
    """index -> packet header index for each dword (walking type-4/7 packets from 0)"""
    owner, i = {}, 0
    while i < len(d):
        h = d[i]
        t = h >> 28
        n = h & 0x3fff if t == 7 else h & 0x7f if t == 4 else 0
        for k in range(i, min(len(d), i + 1 + n)):
            owner[k] = i
        i += 1 + n
    return owner


def main():
    a, b = Cap(sys.argv[1]), Cap(sys.argv[2])
    mx = int(sys.argv[sys.argv.index('--max') + 1]) if '--max' in sys.argv else 12
    print('submits: %d vs %d; objects allocated: %d vs %d' % (len(a.submits), len(b.submits), len(a.order), len(b.order)))
    for n, (wa, wb) in enumerate(zip(a.waits, b.waits)):
        print('submit %d: rc %d/%d wait %d  vs  rc %d/%d wait %d' % (n, wa[3], wa[4], 0, wb[3], wb[4], 0))
    for n in range(min(len(a.before), len(b.before))):
        ib_a = {a.where(g)[1]: (a.where(g)[2], s) for g, s, f in (a.submits[n] if n < len(a.submits) else []) if a.where(g)}
        diffs = 0
        for k in sorted(set(a.before[n]) | set(b.before[n])):
            if k not in a.before[n] or k not in b.before[n]:
                print('submit %d obj %d: present only in %s' % (n, k, 'linux' if k in a.before[n] else 'ios'))
                continue
            ba, bb = a.before[n][k], b.before[n][k]
            m = min(len(ba), len(bb)) // 4 * 4
            da = list(struct.unpack('<%dI' % (m // 4), ba[:m]))
            db = list(struct.unpack('<%dI' % (m // 4), bb[:m]))
            na, nb = norm(a, da), norm(b, db)
            bad = [i for i in range(len(na)) if na[i] != nb[i]]
            if len(ba) != len(bb):
                print('submit %d obj %d: size %d vs %d' % (n, k, len(ba), len(bb)))
            if not bad:
                continue
            diffs += 1
            ib = ' (IB %d bytes at +0x%x)' % (ib_a[k][1], ib_a[k][0]) if k in ib_a else ''
            print('submit %d obj %d%s: %d dwords differ, first at +0x%x' % (n, k, ib, len(bad), bad[0] * 4))
            own = headers(da[ib_a[k][0] // 4:(ib_a[k][0] + ib_a[k][1]) // 4]) if k in ib_a else {}
            base = ib_a[k][0] // 4 if k in ib_a else 0
            for i in bad[:mx]:
                h = own.get(i - base)
                ctx = (' in %s at +0x%x' % (pkt(da[base + h]), (base + h) * 4)) if h is not None and pkt(da[base + h]) else ''
                print('    +0x%05x linux %08x  ios %08x%s' % (i * 4, da[i], db[i], ctx))
        if not diffs:
            print('submit %d: every object identical (addresses normalised)' % n)


if __name__ == '__main__':
    main()
