#!/usr/bin/env python3
"""Android sparse image of the 1 MiB chunks where NEW differs from the phone.

  chunkdiff-simg.py NEW.img NEW.chunks PHONE.chunks OUT.simg

NEW.chunks and PHONE.chunks are tools/chunkhash lists ("index hash") of the
new image and of the phone's partition as it is now (chunkhash run in TWRP).
Chunks whose hashes differ become RAW chunks with NEW's data, the rest
DONT_CARE, so `fastboot flash userdata OUT.simg` leaves the phone equal to
NEW without copying the phone's contents to the PC first.
"""
import struct, sys

MAGIC, RAW, DONT_CARE = 0xed26ff3a, 0xcac1, 0xcac3
BS = 4096
CHUNK = 1 << 20
PER = CHUNK // BS


def load(path):
    return [line.split()[1] for line in open(path) if line.strip()]


def main():
    new_img, new_list, phone_list, out = sys.argv[1:5]
    a, b = load(new_list), load(phone_list)
    assert len(a) == len(b), 'chunk lists differ in length'
    runs, i = [], 0
    while i < len(a):
        same = a[i] == b[i]
        j = i
        while j < len(a) and (a[j] == b[j]) == same:
            j += 1
        runs.append((same, i, j - i))
        i = j
    changed = sum(n for same, _, n in runs if not same)
    with open(new_img, 'rb') as src, open(out, 'wb') as f:
        f.write(struct.pack('<IHHHHIIII', MAGIC, 1, 0, 28, 12, BS, len(a) * PER, len(runs), 0))
        for same, start, n in runs:
            if same:
                f.write(struct.pack('<HHII', DONT_CARE, 0, n * PER, 12))
            else:
                f.write(struct.pack('<HHII', RAW, 0, n * PER, 12 + n * CHUNK))
                src.seek(start * CHUNK)
                left = n * CHUNK
                while left:
                    buf = src.read(min(left, 8 * CHUNK))
                    f.write(buf)
                    left -= len(buf)
    print(f'{out}: {changed} of {len(a)} MiB differ, {sum(1 for s, _, _ in runs if not s)} runs')


if __name__ == '__main__':
    main()
