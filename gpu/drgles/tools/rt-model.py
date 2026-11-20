#!/usr/bin/env python3
"""Which render-target bug gives exactly N right texels? (boot #55/#56: 1,324,512 of 1125x2436)

  rt-model.py [N [W H [BPR [VA SIZE]]]]      defaults: 1324512 1125 2436 4608 0x103780000 0xab8000

Each family below is a model of which texels end up right; the script prints every member of a
family whose count is exactly N. Families:
  rect / rect-far   right = one rectangle at the origin / at the far corner
  union             right = x < X or y < Y (and the complements)
  bins              GMEM bins bw x bh (multiples of 16, at most 2x GMEM 0x44000 at 4 B/px): the first
                    or last n bins in row or column order, every other bin/row/column of bins
  addr              right = a byte prefix/suffix of the surface, or the 2 MiB-aligned GPU VA pieces
  pages             right = a fixed subset of 4K..1M sub-pages
Rejected so far by these (2026-11-20): address prefixes, 2 MiB pieces, page subsets, value
coincidences (R/B swap: ~10k). Matches: rectangles 864x1533 etc. (no natural bounds), and bin
prefixes in row order, e.g. 288x224 bins (4 x 11, the size a freedreno-like choice gives for
GMEM 0x44000: 1125/4 -> 288, 278528/(288*4) -> 224), bins 0-20 drawn = rows 0-1119 + one bin.
The self-test's map (rt_report) tells these apart in one run.
"""
import itertools
import sys


def main():
    a = sys.argv[1:]
    N = int(a[0], 0) if a else 1324512
    W, H = (int(a[1]), int(a[2])) if len(a) > 2 else (1125, 2436)
    BPR = int(a[3]) if len(a) > 3 else 4608
    VA, SIZE = (int(a[4], 0), int(a[5], 0)) if len(a) > 5 else (0x103780000, 0xab8000)
    T = W * H
    print(f"N {N} of {T} ({100.0 * N / T:.2f} %), wrong {T - N}")

    for X in range(1, W + 1):
        for want, name in ((N, "right"), (T - N, "wrong")):
            if want % X == 0 and want // X <= H:
                print(f"rect: {name} = {X} x {want // X} rectangle (origin or far corner)")
            r = want - X * H
            if r >= 0 and W - X > 0 and r % (W - X) == 0 and r // (W - X) <= H:
                print(f"union: {name} = x < {X} or y < {r // (W - X)}")

    G = 0x44000
    for bw in range(16, W + 16, 16):
        for bh in range(16, H + 16, 16):
            if bw * bh * 4 > 2 * G:
                continue
            xs = [(x, min(x + bw, W)) for x in range(0, W, bw)]
            ys = [(y, min(y + bh, H)) for y in range(0, H, bh)]
            area = lambda xr, yr: (xr[1] - xr[0]) * (yr[1] - yr[0])
            for order in ("row", "col"):
                bins = [(x, y) for y in ys for x in xs] if order == "row" else [(x, y) for x in xs for y in ys]
                sizes = [area(x, y) for x, y in bins]
                for tag, seq in (("first", sizes), ("last", sizes[::-1])):
                    s = 0
                    for k, v in enumerate(seq):
                        s += v
                        if s == N:
                            print(f"bins: {bw}x{bh} ({len(xs)} x {len(ys)}), {tag} {k + 1} of {len(sizes)} in {order} order")
            for par in (0, 1):
                for tag, sel in (("checker", lambda i, j: (i + j) % 2 == par), ("columns", lambda i, j: i % 2 == par),
                                 ("rows", lambda i, j: j % 2 == par)):
                    s = sum(area(x, y) for i, x in enumerate(xs) for j, y in enumerate(ys) if sel(i, j))
                    if s == N:
                        print(f"bins: {bw}x{bh}, every other ({tag}, parity {par})")

    # byte offsets of the texels, by row: texels of row y with offset < L
    def below(L):
        n = 0
        for y in range(H):
            o = y * BPR
            if o >= L:
                break
            n += min(W, (L - o + 3) // 4)
        return n
    # some byte prefix always has N texels; it counts only if its end is cache-line aligned or better
    for want, name in ((N, "right = prefix"), (T - N, "right = suffix")):
        lo, hi = 0, H * BPR
        while lo < hi:
            m = (lo + hi) // 2
            if below(m) < want:
                lo = m + 1
            else:
                hi = m
        ends = [L for L in range(lo, lo + 4) if below(L) == want]
        al = max((L & -L) for L in ends) if ends and min(ends) else 0
        print(f"addr: {name}: bytes below {lo:#x}..{ends[-1] if ends else lo:#x}, best alignment {al:#x}"
              + ("" if al >= 0x40 else " (unaligned: not a cache-line/page boundary, rejected)"))
    cuts = [0] + [c - VA for c in range((VA // 0x200000 + 1) * 0x200000, VA + SIZE, 0x200000)] + [SIZE]
    pieces = [below(cuts[i + 1]) - below(cuts[i]) for i in range(len(cuts) - 1)]
    for r in range(len(pieces) + 1):
        for c in itertools.combinations(range(len(pieces)), r):
            if sum(pieces[i] for i in c) == N:
                print(f"addr: 2 MiB VA pieces {c} of {[hex(x) for x in cuts]}")
    for pg in (0x1000, 0x4000, 0x10000, 0x20000, 0x80000, 0x100000):
        for mod in (2, 4, 8):
            cnt = [0] * mod
            for y in range(H):
                for x in range(W):
                    cnt[((y * BPR + x * 4 + VA % (pg * mod)) // pg) % mod] += 1
            for r in range(1, mod):
                for c in itertools.combinations(range(mod), r):
                    if sum(cnt[i] for i in c) == N:
                        print(f"pages: sub-pages {c} of every {mod} x {pg:#x}")


if __name__ == "__main__":
    main()
