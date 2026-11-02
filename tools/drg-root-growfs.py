#!/usr/bin/env python3
"""Put the volume grower (tools/ios/drg-growfs.c) into the iOS root.

  drg-root-growfs.py MOUNTED_ROOT DRG_GROWFS

Adds /usr/local/bin/drg-growfs and launchd job com.drg.growfs (root, once per
boot), output to /private/var/log/growfs.log. It grows / to the userdata
partition minus the shim's 64 MiB scratch area, and does nothing once / has
that size. Also removes the GPU bench jobs (com.drg.gpubench*): until the GPU
fault is fixed they only cause recoveries. Idempotent.
"""
import glob, os, plistlib, shutil, sys

LABEL = 'com.drg.growfs'
KEY = '/System/Library/LaunchDaemons/' + LABEL + '.plist'
JOB = {
    'Label': LABEL,
    'ProgramArguments': ['/usr/local/bin/drg-growfs'],
    'RunAtLoad': True,
    'LaunchOnlyOnce': True,
    'KeepAlive': False,
    'UserName': 'root',
    'LimitLoadToSessionType': 'System',
    'StandardOutPath': '/private/var/log/growfs.log',
    'StandardErrorPath': '/private/var/log/growfs.log',
}
JETSAM = 'System/Library/LaunchDaemons/com.apple.jetsamproperties.D22.plist'


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
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    root, tool = sys.argv[1:]
    dst = os.path.join(root, 'usr/local/bin/drg-growfs')
    os.makedirs(os.path.dirname(dst), mode=0o755, exist_ok=True)
    shutil.copyfile(tool, dst)
    os.chown(dst, 0, 0)
    os.chmod(dst, 0o755)

    def jobs(p):
        d = p['LaunchDaemons']
        for k in [k for k in d if '/com.drg.gpubench' in k]:
            del d[k]
        d[KEY] = JOB
    plist = rewrite(os.path.join(root, 'System/Library/xpc/launchd.plist'), jobs)
    for f in glob.glob(os.path.join(root, 'System/Library/LaunchDaemons/com.drg.gpubench*.plist')):
        os.remove(f)
    job = os.path.join(root, KEY.lstrip('/'))
    with open(job, 'wb') as f:
        plistlib.dump(JOB, f, fmt=plistlib.FMT_BINARY)
    os.chown(job, 0, 0)
    os.chmod(job, 0o644)

    def limits(p):
        o = p['Version4']['Daemon']['Override']
        for k in [k for k in o if k.startswith('com.drg.gpubench')]:
            del o[k]
        o[LABEL] = {'JetsamPriority': 180, 'ActiveSoftMemoryLimit': 16, 'InactiveHardMemoryLimit': 16}
    rewrite(os.path.join(root, JETSAM), limits)
    left = [k for k in plist['LaunchDaemons'] if 'com.drg.' in k]
    print(f'drg-growfs + job {LABEL}; drg jobs now: {left} ({len(plist["LaunchDaemons"])} jobs)')


if __name__ == '__main__':
    main()
