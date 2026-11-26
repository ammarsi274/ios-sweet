#!/sbin/sh
# Run ON THE PHONE in TWRP (RAM boot of twrp/twrp-drg-tas-ram.img = TWRP + TAS2557 firmware in the ramdisk):
# start Nokia's ADSP and modem the way Android does, with Nokia's own helpers from vendor_b (pushed to /tmp/qc by
# drg-twrp-qc-push.sh). Measured 2026-11-26: ADSP -> sound card sdm660-snd-card-drg appears once pd-mapper
# answers the service locator (cmdline service_locator.enable=1); modem stays up with rmt_storage + tftp_server
# (without them it crashes about every 40 s, reason "unknown"). Nothing is flashed; rmt_storage writes the modem's
# own EFS copies (modemst1/2) as Android does (backup/efs-2026-11-26).
set -x
mount -t vfat -o ro,shortname=lower /dev/block/bootdevice/by-name/modem_a /firmware
mkdir -p /dsp /vendor/firmware /vendor/firmware_mnt
mount -t ext4 -o ro /dev/block/bootdevice/by-name/dsp_a /dsp
mount --bind /firmware /vendor/firmware_mnt
cp /firmware/image/adsp.* /firmware/image/modem.* /firmware/image/mba.mbn /firmware/image/wlanmdsp.mbn /vendor/firmware/
for s in /sys/bus/msm_subsys/devices/subsys*; do echo RELATED > $s/restart_level; done	# a crash restarts only that DSP
mkdir -p /system/bin; ln -sf /sbin/linker64 /system/bin/linker64	# vendor binaries' interpreter
cd /tmp/qc
LD_LIBRARY_PATH=/tmp/qc:/sbin setsid ./pd-mapper > /tmp/pdm.log 2>&1 < /dev/null &
setsid sh -c "sleep 100000 < /dev/subsys_adsp" > /dev/null 2>&1 < /dev/null &
LD_LIBRARY_PATH=/tmp/qc:/sbin setsid ./rmt_storage > /tmp/rmt.log 2>&1 < /dev/null &
LD_LIBRARY_PATH=/tmp/qc:/sbin setsid ./tftp_server > /tmp/tftp.log 2>&1 < /dev/null &
setsid sh -c "sleep 100000 < /dev/subsys_modem" > /dev/null 2>&1 < /dev/null &
sleep 15
cat /sys/bus/msm_subsys/devices/subsys*/name /sys/bus/msm_subsys/devices/subsys*/state
cat /proc/asound/cards
