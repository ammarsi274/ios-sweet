#!/usr/bin/env python3
"""Put the GPU bench (gate I4, drg/GPU-I4-PLAN.md) into the iOS root.

  drg-root-gpubench.py MOUNTED_ROOT BENCH NOKIA_LIBS_DIR

  BENCH           the iOS drgles bench (gpu/drgles/build-ios.sh)
  NOKIA_LIBS_DIR  Nokia's vendor_b /vendor/lib64 copy (research/vendor-adreno/files/lib64):
                  its *.so and egl/*.so go flat into one directory, as on the TWRP bench

Adds:
  /usr/local/bin/drgles-bench
  /usr/local/drgles/adreno/*.so
  launchd jobs com.drg.gpubench{,2,3} (RUNS): the bench once each at boot,
  output to /private/var/log/drgles{,2,3}.log (pulled from TWRP afterwards)
Idempotent. root:wheel, 0755 programs and directories, 0644 libraries.
"""
import glob, os, plistlib, shutil, sys

RUNS = [
    # Boot #41, completion trace: #40's plain run read the previous frame back after glFinish (GPU work
    # identical to Linux). -trace logs the memstore sop/eop and the driver's retire state at every
    # submit and after each glFinish/readback (Linux reference: logs/twrp-trace.txt). Trace at boot,
    # plain at +60 s (does the failure repeat), trace at +120 s.
    ('com.drg.gpubench', ['-trace'], 'drgles.log'),
    ('com.drg.gpubench2', ['-delay', '60'], 'drgles2.log'),
    ('com.drg.gpubench3', ['-delay', '120', '-trace'], 'drgles3.log'),
]


def job(label, args, log):
    return {
        'Label': label,
        'ProgramArguments': ['/usr/local/bin/drgles-bench'] + args + ['/usr/local/drgles/adreno'],
        'RunAtLoad': True,
        'LaunchOnlyOnce': True,
        'KeepAlive': False,
        'UserName': 'root',
        'LimitLoadToSessionType': 'System',
        'POSIXSpawnType': 'Interactive',
        'StandardOutPath': '/private/var/log/' + log,
        'StandardErrorPath': '/private/var/log/' + log,
    }

def put(src, dst, mode):
    os.makedirs(os.path.dirname(dst), mode=0o755, exist_ok=True)
    shutil.copyfile(src, dst)
    os.chown(dst, 0, 0)
    os.chmod(dst, mode)


JETSAM = 'System/Library/LaunchDaemons/com.apple.jetsamproperties.D22.plist'


def jetsam(root, label, limits):
    """Daemon memory limits (MB, -1 = none) in the iPhone X (D22) jetsam file
    launchd reads; without an entry a daemon gets 6 MB (boot #34: the bench
    was killed with "exceeded mem limit: InactiveHard 6 MB")."""
    path = os.path.join(root, JETSAM)
    st = os.stat(path)
    with open(path, 'rb') as f:
        props = plistlib.load(f)
    props['Version4']['Daemon']['Override'][label] = limits
    with open(path, 'wb') as f:
        plistlib.dump(props, f, fmt=plistlib.FMT_BINARY)
    os.chown(path, st.st_uid, st.st_gid)
    os.chmod(path, st.st_mode & 0o7777)
    print(f'jetsam {label}: {limits}')


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    root, bench, libs = sys.argv[1:]
    put(bench, os.path.join(root, 'usr/local/bin/drgles-bench'), 0o755)
    sos = sorted(glob.glob(os.path.join(libs, '*.so')) + glob.glob(os.path.join(libs, 'egl', '*.so')))
    for so in sos:
        put(so, os.path.join(root, 'usr/local/drgles/adreno', os.path.basename(so)), 0o644)
    for d in ('usr/local', 'usr/local/bin', 'usr/local/drgles', 'usr/local/drgles/adreno'):
        os.chown(os.path.join(root, d), 0, 0)
        os.chmod(os.path.join(root, d), 0o755)
    print(f'bench + {len(sos)} Nokia libraries in /usr/local')

    # the jobs, in launchd's cache and as their own plists
    path = os.path.join(root, 'System/Library/xpc/launchd.plist')
    st = os.stat(path)
    with open(path, 'rb') as f:
        plist = plistlib.load(f)
    for label, args, log in RUNS:
        key = '/System/Library/LaunchDaemons/' + label + '.plist'
        plist['LaunchDaemons'][key] = job(label, args, log)
        with open(os.path.join(root, key.lstrip('/')), 'wb') as f:
            plistlib.dump(job(label, args, log), f, fmt=plistlib.FMT_BINARY)
        os.chown(os.path.join(root, key.lstrip('/')), 0, 0)
        os.chmod(os.path.join(root, key.lstrip('/')), 0o644)
    with open(path, 'wb') as f:
        plistlib.dump(plist, f, fmt=plistlib.FMT_BINARY)
    os.chown(path, st.st_uid, st.st_gid)
    os.chmod(path, st.st_mode & 0o7777)
    print(f'jobs {[r[0] for r in RUNS]} in launchd.plist ({len(plist["LaunchDaemons"])} jobs)')
    for label, _, _ in RUNS:
        jetsam(root, label, {'JetsamPriority': 180, 'ActiveSoftMemoryLimit': -1, 'InactiveHardMemoryLimit': -1})

if __name__ == '__main__':
    main()
