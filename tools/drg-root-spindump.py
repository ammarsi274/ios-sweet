#!/usr/bin/env python3
"""Sample SpringBoard and the rest of the system with Apple's spindump at boot.

  drg-root-spindump.py MOUNTED_ROOT

Boots #32-#38 stay on the Apple logo: backboardd draws it and waits for
SpringBoard, which never reports ready (no crash; not the bench, #37; not
DrgGPU, #38). spindump (/usr/sbin/spindump, run by launchd in those boots)
samples every thread and names what each one is blocked on.

Adds launchd job com.drg.spindump: every 150 s, 5 s of samples at 10 ms with
SpringBoard as the target (all processes are kept), appended to
/private/var/log/spindump.txt (pulled from TWRP). Creates
/private/var/db/.spindump_enable_symbolication, without which iOS's spindump
writes addresses only. Idempotent.
"""
import os, plistlib, sys

LABEL = 'com.drg.spindump'
KEY = '/System/Library/LaunchDaemons/' + LABEL + '.plist'
JOB = {
    'Label': LABEL,
    'ProgramArguments': ['/usr/sbin/spindump', 'SpringBoard', '5', '10', '-stdout'],
    'StartInterval': 150,
    'RunAtLoad': False,
    'UserName': 'root',
    'LimitLoadToSessionType': 'System',
    'StandardOutPath': '/private/var/log/spindump.txt',
    'StandardErrorPath': '/private/var/log/spindump.txt',
}
JETSAM = 'System/Library/LaunchDaemons/com.apple.jetsamproperties.D22.plist'
FLAG = 'private/var/db/.spindump_enable_symbolication'


def rewrite(path, change):
    st = os.stat(path)
    with open(path, 'rb') as f:
        p = plistlib.load(f)
    change(p)
    with open(path, 'wb') as f:
        plistlib.dump(p, f, fmt=plistlib.FMT_BINARY)
    os.chown(path, st.st_uid, st.st_gid)
    os.chmod(path, st.st_mode & 0o7777)
    return p


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    root = sys.argv[1]
    plist = rewrite(os.path.join(root, 'System/Library/xpc/launchd.plist'),
                    lambda p: p['LaunchDaemons'].__setitem__(KEY, JOB))
    job = os.path.join(root, KEY.lstrip('/'))
    with open(job, 'wb') as f:
        plistlib.dump(JOB, f, fmt=plistlib.FMT_BINARY)
    os.chown(job, 0, 0)
    os.chmod(job, 0o644)
    print(f'job {LABEL} ({len(plist["LaunchDaemons"])} jobs in launchd.plist)')
    limits = {'JetsamPriority': 180, 'ActiveSoftMemoryLimit': -1, 'InactiveHardMemoryLimit': -1}
    rewrite(os.path.join(root, JETSAM), lambda p: p['Version4']['Daemon']['Override'].__setitem__(LABEL, limits))
    print(f'jetsam {LABEL}: {limits}')
    flag = os.path.join(root, FLAG)
    open(flag, 'wb').close()
    os.chown(flag, 0, 0)
    os.chmod(flag, 0o644)
    print(f'/{FLAG}')


if __name__ == '__main__':
    main()
