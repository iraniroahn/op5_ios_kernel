#!/bin/bash
# One command per shim test on the Mac: ./fbtest.sh touch4-xxxxxxxx.img
# Works from fastbootd (recovery's fastboot) or the bootloader; if the phone
# is still on a red test page, hold power + vol up and it continues by itself.
set -e
img=${1:?usage: fbtest.sh IMAGE.img}
[ -f "$img" ] || { echo "no file $img (run this in the folder that holds it)"; exit 1; }
if fastboot getvar is-userspace 2>&1 | grep -q "is-userspace: yes"; then
	fastboot reboot bootloader
	sleep 3
fi
fastboot boot "$img"
