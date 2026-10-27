#!/usr/bin/env python3
"""Remove launchd jobs the Nokia 6.1 Plus (DRG) has no hardware for.

  drg-root-jobs.py MOUNTED_ROOT

MOUNTED_ROOT is the iOS root volume mounted read-write. Each job below was
measured on the phone first (drg/BOOTLOG.md boot #16, launchd's own log in
/private/var/db/diagnostics): it found no hardware, exited with exit(0)
and, being KeepAlive, was started again every ~10 s (about 250 times in 45
minutes), each time a full process launch on a CPU that emulates the
iPhone's LSE atomics in the kernel. Idempotent; keeps owner and mode.
"""
import os, plistlib, sys

JOBS = {
    'com.apple.bluetoothd': 'no Bluetooth driver for the Nokia in this kernel: '
                            '254 starts, each exit(0)',
    'com.apple.fairplayd.H2': 'needs the iPhone Secure Enclave (FairPlay), absent on the '
                              'SDM636: 252 starts, each exit(0)',
}


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    path = os.path.join(sys.argv[1], 'System/Library/xpc/launchd.plist')
    st = os.stat(path)
    with open(path, 'rb') as f:
        plist = plistlib.load(f)
    jobs = plist['LaunchDaemons']
    removed = []
    for key in [k for k, j in jobs.items() if j.get('Label') in JOBS]:
        removed.append(jobs.pop(key).get('Label'))
    with open(path, 'wb') as f:
        plistlib.dump(plist, f, fmt=plistlib.FMT_BINARY)
    os.chown(path, st.st_uid, st.st_gid)
    os.chmod(path, st.st_mode & 0o7777)
    for label in JOBS:
        state = 'removed' if label in removed else 'not present'
        print(f'{label}: {state} ({JOBS[label]})')
    print(f'launchd.plist: {len(jobs)} jobs left')


if __name__ == '__main__':
    main()
