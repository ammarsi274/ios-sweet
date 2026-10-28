#!/usr/bin/env python3
"""Fit the iOS root to the Nokia 6.1 Plus (DRG): jobs, extensions, settings.

  drg-root-jobs.py MOUNTED_ROOT

MOUNTED_ROOT is the iOS root volume mounted read-write. Every change below
was measured on the phone first (drg/BOOTLOG.md, launchd's own log and the
crash reports from /private/var), and each carries its reason. On a CPU
that emulates the iPhone's LSE atomics in the kernel, every pointless
process launch is expensive. Idempotent; keeps owners and modes.
"""
import os, plistlib, sys

MOBILE = 501

# launchd jobs removed from /System/Library/xpc/launchd.plist
JOBS = {
    'com.apple.bluetoothd': 'no Bluetooth driver for the Nokia in this kernel: '
                            '254 starts in boot #16, each exit(0)',
    'com.apple.fairplayd.H2': 'needs the iPhone Secure Enclave (FairPlay), absent on the '
                              'SDM636: 252 starts in boot #16, each exit(0)',
    'com.apple.nanotimekitcompaniond': 'Apple Watch faces; no Watch can pair without Bluetooth: '
                                       '89 starts in boot #17, each exit(0)',
    'com.apple.voicemail.vmd': 'visual voicemail needs the modem (CommCenter, absent): 402,419 '
                               'failed lookups of com.apple.commcenter.coretelephony.xpc in boot #18',
}

# bundles renamed to <name>.drg-off, so no registry or plug-in scan finds them
EXTENSIONS = {
    'System/Library/Audio/Plug-Ins/HAL/BTAudioHALPlugin.driver':
        'Bluetooth audio HAL plug-in in mediaserverd; with no Bluetooth it retried its XPC '
        'service in a loop: 404,055 failed lookups and 3,098,686 "Connection invalid" in boot #18',
    'System/Library/CoreServices/AegirProxyApp.app/PlugIns/AegirPoster.appex':
        'Astronomy wallpaper renders with Metal (NanoUniverse/ClockKitUI); no Metal device: '
        '84 starts in boot #17, each NSInvalidArgumentException (nil uniform buffer)',
    'System/Library/PrivateFrameworks/TextToSpeechKonaSupport.framework/PlugIns/KonaSynthesizer.appex':
        'speech voice, no audio driver; 21 starts in boot #17, each "plugin has invalid '
        'sandbox or container: error 2"',
    'System/Library/PrivateFrameworks/MacinTalk.framework/PlugIns/MacinTalkAUSP.appex':
        'speech voice, no audio driver; 19 starts in boot #17, same sandbox container error',
    'System/Library/PrivateFrameworks/SiriTTSService.framework/PlugIns/SiriTTSSynthesizerAU.appex':
        'speech voice, no audio driver; 19 starts in boot #17, same sandbox container error',
}

# /var/mobile/Library/Preferences/<domain>.plist (mobile, 0600)
PREFS = {
    'com.apple.Accessibility': {
        # Reduce Motion: no motion passes for CoreAnimation to draw on the CPU
        # (no GPU for iOS on the SDM636). Reduce Transparency was on in r8/r9;
        # boot #30 showed it turns every blurred material into a flat grey
        # box, so r10 turns it off to see iOS's own blur at this CPU speed.
        # Keys as read by iOS 16.7 (CoreServices, libAccessibility).
        'ReduceMotionEnabled': 1,
        'EnhancedBackgroundContrastEnabled': 0,
    },
}


def jobs(root):
    path = os.path.join(root, 'System/Library/xpc/launchd.plist')
    st = os.stat(path)
    with open(path, 'rb') as f:
        plist = plistlib.load(f)
    daemons = plist['LaunchDaemons']
    removed = []
    for key in [k for k, j in daemons.items() if j.get('Label') in JOBS]:
        removed.append(daemons.pop(key).get('Label'))
    with open(path, 'wb') as f:
        plistlib.dump(plist, f, fmt=plistlib.FMT_BINARY)
    os.chown(path, st.st_uid, st.st_gid)
    os.chmod(path, st.st_mode & 0o7777)
    for label, why in JOBS.items():
        print(f'job {label}: {"removed" if label in removed else "not present"} ({why})')
    print(f'launchd.plist: {len(daemons)} jobs left')


def extensions(root):
    for rel, why in EXTENSIONS.items():
        src = os.path.join(root, rel)
        dst = src + '.drg-off'
        if os.path.isdir(src):
            os.rename(src, dst)
            state = 'disabled'
        else:
            state = 'already disabled' if os.path.isdir(dst) else 'NOT FOUND'
        print(f'appex {os.path.basename(rel)}: {state} ({why})')


def prefs(root):
    for domain, values in PREFS.items():
        path = os.path.join(root, 'private/var/mobile/Library/Preferences', domain + '.plist')
        data = {}
        if os.path.exists(path):
            with open(path, 'rb') as f:
                data = plistlib.load(f)
        data.update(values)
        with open(path, 'wb') as f:
            plistlib.dump(data, f, fmt=plistlib.FMT_BINARY)
        os.chown(path, MOBILE, MOBILE)
        os.chmod(path, 0o600)
        print(f'pref {domain}: {sorted(values)}')


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    jobs(sys.argv[1])
    extensions(sys.argv[1])
    prefs(sys.argv[1])


if __name__ == '__main__':
    main()
