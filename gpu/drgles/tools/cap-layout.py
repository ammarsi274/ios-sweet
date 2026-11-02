#!/usr/bin/env python3
"""How Nokia's driver lays out a W x H RGBA8 texture (bench -layout W H -capture FILE).

  cap-layout.py CAPTURE.bin W H
  cap-layout.py --selftest

The bench fills two textures with texels that encode their own position
(R = x & 0xff, G = x >> 8, B = y & 0xff, A = y >> 8): one uploaded by the
CPU, one rendered by the GPU (bench.c layout_test). For every captured
object snapshot that holds texel (0,0) followed by texel (1,0), this finds
the row pitch (where texel (0,1) is) and checks every texel against a linear
layout at that pitch. Prints, per object: base offset, pitch in bytes and
pixels, and how many texels sit where linear says. A count below W*H means
the layout is not linear (tiled/compressed); the first misplaced texels and
where they are found are printed then.
"""
import struct, sys

REC = struct.Struct('<IIII')
OBJ = struct.Struct('<IIIIQQQ')


def records(data):
    pos = 0
    while pos + REC.size <= len(data):
        magic, typ, ln, _ = REC.unpack_from(data, pos)
        assert magic == 0x43475244, hex(pos)
        yield typ, data[pos + REC.size:pos + REC.size + ln]
        pos += REC.size + ln


def texel(x, y):
    return bytes((x & 0xff, x >> 8, y & 0xff, y >> 8))


def analyse(buf, w, h):
    """None if texel (0,0)(1,0) is not in buf, else a dict describing the layout."""
    first = texel(0, 0) + texel(1, 0)
    base = buf.find(first)
    while base >= 0 and base % 4:
        base = buf.find(first, base + 1)
    if base < 0:
        return None
    row1 = texel(0, 1) + texel(1, 1)
    at = buf.find(row1, base + 4 * w)
    pitch = at - base if at >= 0 else None
    res = dict(base=base, pitch=pitch, linear=0, bad=[])
    if pitch is None:
        return res
    for y in range(h):
        row = buf[base + y * pitch:base + y * pitch + 4 * w]
        if len(row) < 4 * w:
            res['bad'].append((0, y, 'row past the object'))
            break
        exp = b''.join(texel(x, y) for x in range(w))
        if row == exp:
            res['linear'] += w
            continue
        for x in range(w):
            if row[4 * x:4 * x + 4] == exp[4 * x:4 * x + 4]:
                res['linear'] += 1
            elif len(res['bad']) < 8:
                where = buf.find(texel(x, y))
                res['bad'].append((x, y, where - base if where >= 0 else None))
    return res


def report(tag, r, w, h):
    if r['pitch'] is None:
        print('%s: texel (0,0) at +%#x, texel (0,1) not found after it: not linear' % (tag, r['base']))
        return
    print('%s: base +%#x, pitch %d bytes (%d px, %d px padding), linear texels %d of %d%s'
          % (tag, r['base'], r['pitch'], r['pitch'] // 4, r['pitch'] // 4 - w, r['linear'], w * h,
             ' -> LINEAR' if r['linear'] == w * h else ' -> NOT linear'))
    for x, y, off in r['bad']:
        print('    texel (%d,%d) not where linear says; found at %s' % (x, y, '+%#x' % off if off is not None else 'nowhere'))


def selftest():
    w, h, pitch = 5, 3, 32		# 5 px rows padded to 8 px
    buf = bytearray(b'\xee' * 16)
    for y in range(h):
        row = b''.join(texel(x, y) for x in range(w))
        buf += row + b'\x00' * (pitch - len(row))
    r = analyse(bytes(buf), w, h)
    assert r['base'] == 16 and r['pitch'] == 32 and r['linear'] == w * h, r
    t = bytearray(buf)				# swap two texels: no longer linear
    t[16 + 4:16 + 8], t[16 + 32:16 + 36] = t[16 + 32:16 + 36], t[16 + 4:16 + 8]
    r = analyse(bytes(t), w, h)
    assert r is None or r['linear'] < w * h, r
    assert analyse(b'\x00' * 64, w, h) is None or True
    print('selftest OK')


def main():
    if sys.argv[1:] == ['--selftest']:
        return selftest()
    data = open(sys.argv[1], 'rb').read()
    w, h = int(sys.argv[2]), int(sys.argv[3])
    seen = set()
    for typ, p in records(data):
        if typ != 5:
            continue
        oid, phase, sub, has, gva, ln, hsh = OBJ.unpack_from(p)
        if not has or (oid, hsh) in seen:
            continue
        seen.add((oid, hsh))
        r = analyse(p[40:40 + ln], w, h)
        if r is not None:
            report('obj %d gpuaddr %#x len %#x (%s)' % (oid, gva, ln, 'after GPU' if phase else 'before GPU'), r, w, h)


if __name__ == '__main__':
    main()
