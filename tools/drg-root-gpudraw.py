#!/usr/bin/env python3
"""Put GPU drawing for backboardd into the iOS root (drg/GPU-DRAW-PLAN.md, the one boot).

  drg-root-gpudraw.py MOUNTED_ROOT BENCH LIBDRGEAGL

  BENCH       the iOS drgles bench (gpu/drgles/build-ios.sh), with -selftest
  LIBDRGEAGL  gpu/drgles/build-eagl.sh's libdrgeagl.dylib

Adds or replaces:
  /usr/local/bin/drgles-bench, /usr/local/lib/libdrgeagl.dylib
  launchd job com.drg.gpuselftest: once at boot, as root, `drgles-bench -selftest
    /private/var/run/drg-gpu-mode`, output /private/var/log/drgles-selftest.log
  com.apple.backboardd: EnvironmentVariables DYLD_INSERT_LIBRARIES = the library
    (launchd's cache and the job's own plist; patch 0914 lets dyld honour it)
Nokia's libraries must already be in /usr/local/drgles/adreno (roots r11 and later).
Prints backboardd's jetsam entry (the driver adds memory to it). Idempotent.
"""
import os, plistlib, shutil, sys

LIB = '/usr/local/lib/libdrgeagl.dylib'
SELFTEST = 'com.drg.gpuselftest'
BACKBOARDD = '/System/Library/LaunchDaemons/com.apple.backboardd.plist'
JETSAM = 'System/Library/LaunchDaemons/com.apple.jetsamproperties.D22.plist'


def put(src, dst, mode):
    os.makedirs(os.path.dirname(dst), mode=0o755, exist_ok=True)
    shutil.copyfile(src, dst)
    os.chown(dst, 0, 0)
    os.chmod(dst, mode)


def load(path):
    with open(path, 'rb') as f:
        return plistlib.load(f)


def save(path, obj, like=None):
    st = os.stat(like or path) if os.path.exists(like or path) else None
    with open(path, 'wb') as f:
        plistlib.dump(obj, f, fmt=plistlib.FMT_BINARY)
    os.chown(path, st.st_uid if st else 0, st.st_gid if st else 0)
    os.chmod(path, (st.st_mode & 0o7777) if st else 0o644)


def selftest_job():
    return {
        'Label': SELFTEST,
        'ProgramArguments': ['/usr/local/bin/drgles-bench', '-selftest', '/private/var/run/drg-gpu-mode'],
        'RunAtLoad': True,
        'LaunchOnlyOnce': True,
        'KeepAlive': False,
        'UserName': 'root',
        'LimitLoadToSessionType': 'System',
        'POSIXSpawnType': 'Interactive',
        'StandardOutPath': '/private/var/log/drgles-selftest.log',
        'StandardErrorPath': '/private/var/log/drgles-selftest.log',
    }


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    root, bench, lib = sys.argv[1:]
    adreno = os.path.join(root, 'usr/local/drgles/adreno')
    sos = sorted(f for f in os.listdir(adreno) if f.endswith('.so')) if os.path.isdir(adreno) else []
    if 'libGLESv2_adreno.so' not in sos or 'libEGL_adreno.so' not in sos:
        sys.exit('error: Nokia libraries missing in /usr/local/drgles/adreno')
    put(bench, os.path.join(root, 'usr/local/bin/drgles-bench'), 0o755)
    put(lib, os.path.join(root, LIB.lstrip('/')), 0o755)
    os.chown(os.path.join(root, 'usr/local/lib'), 0, 0)
    os.chmod(os.path.join(root, 'usr/local/lib'), 0o755)
    print(f'bench, {LIB}; {len(sos)} Nokia libraries already there')

    path = os.path.join(root, 'System/Library/xpc/launchd.plist')
    plist = load(path)
    jobs = plist['LaunchDaemons']
    key = '/System/Library/LaunchDaemons/' + SELFTEST + '.plist'
    jobs[key] = selftest_job()
    save(os.path.join(root, key.lstrip('/')), selftest_job(), like=os.path.join(root, BACKBOARDD.lstrip('/')))
    bb = jobs[BACKBOARDD]
    bb.setdefault('EnvironmentVariables', {})['DYLD_INSERT_LIBRARIES'] = LIB
    save(path, plist)
    own = os.path.join(root, BACKBOARDD.lstrip('/'))
    b2 = load(own)
    b2.setdefault('EnvironmentVariables', {})['DYLD_INSERT_LIBRARIES'] = LIB
    save(own, b2)
    print(f'{SELFTEST} added; backboardd job now: {bb}')

    jet = load(os.path.join(root, JETSAM))['Version4']
    for kind in ('Daemon', 'Extension'):
        for k, v in jet.get(kind, {}).get('Override', {}).items():
            if 'backboardd' in k:
                print(f'jetsam {kind} {k}: {v}')
    d = jet.get('Daemon', {}).get('Override', {})
    if SELFTEST not in d:
        d[SELFTEST] = {'JetsamPriority': 180, 'ActiveSoftMemoryLimit': -1, 'InactiveHardMemoryLimit': -1}
        props = load(os.path.join(root, JETSAM))
        props['Version4']['Daemon']['Override'][SELFTEST] = d[SELFTEST]
        save(os.path.join(root, JETSAM), props)
        print(f'jetsam {SELFTEST}: {d[SELFTEST]}')


if __name__ == '__main__':
    main()
