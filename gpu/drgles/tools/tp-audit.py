#!/usr/bin/env python3
"""Audit every `mrs Xt, TPIDR_EL0` in an AArch64 ELF (llvm-objdump text on stdin).

For each site, follow the instructions after it (straight line, until Xt is
overwritten, a branch/return ends the block, or 400 instructions): every
read of Xt (or of a register Xt was copied into with `mov`) must be a load
`[Xt, #0x28]` (bionic's stack-guard slot). Prints sites that do anything
else. Branches are not followed, so a register that is still live past a
branch is reported as "live at branch" for a closer look.
"""
import re, sys
from collections import Counter

ins = []
for line in sys.stdin:
    m = re.match(r'\s*([0-9a-f]+):\s+(\S+)\s*(.*)$', line.rstrip())
    if m:
        ins.append((int(m.group(1), 16), m.group(2).lower(), m.group(3).split('//')[0].strip()))

def regs(ops):
    return re.findall(r'\b([xw])(\d+)\b', ops)

stats, bad = Counter(), []
for i, (addr, mn, ops) in enumerate(ins):
    if mn != 'mrs' or 'tpidr_el0' not in ops.lower():
        continue
    tracked = {re.match(r'x(\d+)', ops).group(1)}
    verdict = 'end of window'
    for a2, m2, o2 in ins[i + 1:i + 401]:
        rs = regs(o2)
        dest = rs[0][1] if rs and m2 not in ('str', 'stp', 'stur', 'cmp', 'cbz', 'cbnz', 'tbz', 'tbnz', 'b', 'br', 'blr', 'ret') and not m2.startswith(('b.', 'st')) else None
        uses = {n for k, n in rs[1:]} if dest else {n for k, n in rs}
        hit = uses & tracked
        if hit:
            if re.search(r'\[x(%s), #0x28\]' % '|'.join(hit), o2) and m2 in ('ldr', 'ldur'):
                stats['ldr #0x28'] += 1
            elif m2 == 'mov' and dest:
                tracked.add(dest)
                stats['mov copy'] += 1
            else:
                verdict = f'other use: {a2:x} {m2} {o2}'
                break
        if dest in tracked and not (m2 == 'mov' and hit):
            tracked.discard(dest)
            if not tracked:
                verdict = 'overwritten'
                break
        if m2 in ('ret', 'b', 'br', 'blr', 'bl') or m2.startswith('b.') or m2 in ('cbz', 'cbnz', 'tbz', 'tbnz'):
            if m2 in ('bl', 'blr'):
                # calls clobber x0-x18 only
                tracked = {t for t in tracked if int(t) >= 19}
                if not tracked:
                    verdict = 'clobbered by call'
                    break
                continue
            verdict = 'live at branch'
            break
    stats[verdict.split(':')[0]] += 1
    if verdict.startswith('other'):
        bad.append(f'{addr:x}: {verdict}')
print('sites:', sum(v for k, v in stats.items() if k not in ('ldr #0x28', 'mov copy')), dict(stats))
for b in bad[:40]:
    print(b)
