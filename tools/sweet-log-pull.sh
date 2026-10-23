#!/bin/sh
# Read the SWEET console log (kernel patch 0047): the last 1025 blocks
# (4 KiB) of the iOS root disk. Read-only.
#   tools/sweet-log-pull.sh OUT.txt              root on userdata: phone in a
#                                                recovery with adb root (OrangeFox/TWRP)
#   tools/sweet-log-pull.sh OUT.txt container    root in /data/ios/root.img (patch 0084):
#                                                phone back in rooted Android (su)
set -eu
out=${1:?usage: sweet-log-pull.sh OUT.txt [container]}
if [ "${2:-}" = container ]; then
	img=/data/ios/root.img
	bytes=$(adb shell "su -c 'stat -c %s $img'" | tr -d '\r')
	skip=$((bytes / 4096 - 1025))
	adb exec-out "su -c 'dd if=$img bs=4096 skip=$skip count=1025 2>/dev/null'" > "$out.bin"
	echo "$img: $bytes bytes, read blocks $skip.. ($(stat -c %s "$out.bin" 2>/dev/null || stat -f %z "$out.bin") bytes)" >&2
else
	dev=$(adb shell 'for d in /dev/block/by-name/userdata /dev/block/bootdevice/by-name/userdata; do [ -e $d ] && readlink -f $d && break; done' | tr -d '\r')
	[ -n "$dev" ] || { echo "userdata block device not found" >&2; exit 1; }
	bytes=$(adb shell blockdev --getsize64 "$dev" | tr -d '\r')
	skip=$((bytes / 4096 - 1025))
	adb exec-out "dd if=$dev bs=4096 skip=$skip count=1025 2>/dev/null" > "$out.bin"
	echo "$dev: $bytes bytes, read blocks $skip.. ($(stat -c %s "$out.bin") bytes)" >&2
fi
python3 "$(dirname "$0")/sweet-log.py" "$out.bin" "$out"
