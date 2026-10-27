#!/usr/bin/env python3
"""Summarise iOS .ips crash reports (bug_type 309): what crashed and why.

  drg-ips.py REPORT.ips...

Prints the process, exception, termination reason, the application-specific
information (asi: abort messages, exception reasons), the last exception
backtrace if any, and the top frames of the faulting thread.
"""
import json, sys


def frames(fr, images, n):
    out = []
    for f in fr[:n]:
        im = images[f['imageIndex']] if f.get('imageIndex') is not None and f['imageIndex'] < len(images) else {}
        sym = f.get('symbol', '')
        out.append(f"    {im.get('name', '?')} +{f.get('imageOffset', 0):#x} {sym} {f.get('symbolLocation', '')}")
    return out


for path in sys.argv[1:]:
    raw = open(path, errors='replace').read()
    head, _, body = raw.partition('\n')
    try:
        d = json.loads(body)
    except ValueError:
        print(path, ': not a JSON crash report'); continue
    print(f"== {d.get('procName')} ({path.split('/')[-1]})")
    print('  exception  ', json.dumps(d.get('exception')))
    print('  termination', json.dumps(d.get('termination')))
    for k in ('asi', 'ktriageinfo'):
        if k in d:
            print(f'  {k:11s}', json.dumps(d[k])[:1500])
    imgs = d.get('usedImages', [])
    if d.get('lastExceptionBacktrace'):
        print('  last exception backtrace:')
        print('\n'.join(frames(d['lastExceptionBacktrace'], imgs, 12)))
    t = d['threads'][d.get('faultingThread', 0)]
    print('  faulting thread:')
    print('\n'.join(frames(t.get('frames', []), imgs, 12)))
