#!/usr/bin/env python3
"""Put the kernel log keeper (tools/ios/drg-kmsg.c) into the iOS root.

  drg-root-kmsg.py MOUNTED_ROOT DRG_KMSG

Adds /usr/local/bin/drg-kmsg and launchd job com.drg.kmsg (root, started at
load and restarted if it exits). It appends the kernel's messages to
/private/var/log/kernel.log, read back from TWRP after a boot, so the
screen's exec / CODE SIGNING / CRASH / driver lines need no photos. Boot
with msgbuf=1048576 so the start of the boot is still in the buffer.
Idempotent.
"""
import os, plistlib, shutil, sys

LABEL = 'com.drg.kmsg'
KEY = '/System/Library/LaunchDaemons/' + LABEL + '.plist'
JOB = {
    'Label': LABEL,
    'ProgramArguments': ['/usr/local/bin/drg-kmsg'],
    'RunAtLoad': True,
    'KeepAlive': True,
    'ThrottleInterval': 5,
    'UserName': 'root',
    'LimitLoadToSessionType': 'System',
    'POSIXSpawnType': 'Interactive',
}


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
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    root, tool = sys.argv[1:]
    dst = os.path.join(root, 'usr/local/bin/drg-kmsg')
    os.makedirs(os.path.dirname(dst), mode=0o755, exist_ok=True)
    shutil.copyfile(tool, dst)
    os.chown(dst, 0, 0)
    os.chmod(dst, 0o755)
    path = os.path.join(root, 'System/Library/xpc/launchd.plist')
    st = os.stat(path)
    with open(path, 'rb') as f:
        plist = plistlib.load(f)
    plist['LaunchDaemons'][KEY] = JOB
    with open(path, 'wb') as f:
        plistlib.dump(plist, f, fmt=plistlib.FMT_BINARY)
    os.chown(path, st.st_uid, st.st_gid)
    os.chmod(path, st.st_mode & 0o7777)
    job = os.path.join(root, KEY.lstrip('/'))
    with open(job, 'wb') as f:
        plistlib.dump(JOB, f, fmt=plistlib.FMT_BINARY)
    os.chown(job, 0, 0)
    os.chmod(job, 0o644)
    print(f'drg-kmsg + job {LABEL} ({len(plist["LaunchDaemons"])} jobs in launchd.plist)')
    jetsam(root, LABEL, {'JetsamPriority': 180, 'ActiveSoftMemoryLimit': 16, 'InactiveHardMemoryLimit': 16})


if __name__ == '__main__':
    main()
