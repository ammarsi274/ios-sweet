#!/usr/bin/env python3
"""Independent check of gptslot_test output (zlib's CRC32, not ours).

  gptslot_check.py IN_HEAD IN_TAIL OUT_HEAD OUT_TAIL WANT_A WANT_B

Both copies: valid header and entry CRCs, entries identical, boot_a/boot_b
attribute byte 6 as wanted, and nothing else changed except the CRC fields.
"""
import struct, sys, zlib

ih, it, oh, ot = (open(p, 'rb').read() for p in sys.argv[1:5])
wa, wb = int(sys.argv[5], 0), int(sys.argv[6], 0)


def hdr_ok(h):
    hs, = struct.unpack_from('<I', h, 12)
    c = bytearray(h[:hs]); c[16:20] = b'\0' * 4
    return zlib.crc32(c) == struct.unpack_from('<I', h, 16)[0]


def view(head, tail):
    ph, bh = head[512:1024], tail[-512:]
    pe, be = head[1024:1024 + 88 * 128], tail[:88 * 128]
    assert hdr_ok(ph) and hdr_ok(bh), 'header CRC'
    assert zlib.crc32(pe) == struct.unpack_from('<I', ph, 88)[0], 'primary entry CRC'
    assert zlib.crc32(be) == struct.unpack_from('<I', bh, 88)[0], 'backup entry CRC'
    assert pe == be, 'copies differ'
    idx = {pe[i * 128 + 56:(i + 1) * 128].decode('utf-16le').rstrip('\0'): i for i in range(88)}
    return pe[idx['boot_a'] * 128 + 54], pe[idx['boot_b'] * 128 + 54], idx


a, b, idx = view(oh, ot)
assert (a, b) == (wa, wb), 'attrs %#x %#x, want %#x %#x' % (a, b, wa, wb)
allowed = set()
for base in (512, 0):	# header CRC fields
    pass
for name in ('boot_a', 'boot_b'):
    allowed.add(1024 + idx[name] * 128 + 54)
allowed |= {512 + 16 + k for k in range(4)} | {512 + 88 + k for k in range(4)}
diff_h = [i for i in range(len(ih)) if ih[i] != oh[i] and i not in allowed]
allowed_t = {idx[n] * 128 + 54 for n in ('boot_a', 'boot_b')}
allowed_t |= {len(it) - 512 + 16 + k for k in range(4)} | {len(it) - 512 + 88 + k for k in range(4)}
diff_t = [i for i in range(len(it)) if it[i] != ot[i] and i not in allowed_t]
assert not diff_h and not diff_t, 'unexpected changes %s %s' % (diff_h[:8], diff_t[:8])
print('check OK: boot_a %#x boot_b %#x, both copies valid, only attribute bytes and CRCs changed' % (a, b))
