#!/bin/bash
# Build the shim and wrap it in a boot.img v0 for the OnePlus 5 (cheeseburger)
# ABL: the kernel is a gzip'd Image with the DTB(s) appended (Image.gz-dtb, as
# the stock and LineageOS kernels ship it); ABL picks the DTB by msm-id/board-id.
#
#   shim/build.sh                 probe image (no payload): shim/out/shim-boot.img
#   shim/build.sh PAYLOAD.bin     xnu payload in the v0 ramdisk (default)
#   EMBED=1 shim/build.sh PAYLOAD.bin   payload appended to the Image behind _end
#                                 instead (topaz style), if ABL drops the ramdisk
#   TOUCH=1 shim/build.sh         probe image with only the touch bring-up page
#   CPU=1 shim/build.sh           probe image with only the gold CPU speed page
#   DTB=path shim/build.sh ...    appended DTB(s); default: the stock boot.img's,
#                                 from tools/unpack_bootv0.py
set -euo pipefail
cd "$(dirname "$0")"
CC=aarch64-linux-gnu-gcc
OBJCOPY=aarch64-linux-gnu-objcopy
CFLAGS="-O2 -ffreestanding -nostdlib -fno-pic -mgeneral-regs-only -mstrict-align -fno-stack-protector -Wall -Wextra"
[ "${TOUCH:-0}" = 1 ] && CFLAGS="$CFLAGS -DTOUCH_PROBE"
[ "${CPU:-0}" = 1 ] && CFLAGS="$CFLAGS -DCPU_PROBE"
SRCS="start.S shim.c console.c mmu.c fdt.c loader.c ufs.c probe.c"
DTB=${DTB:-../dtb/boot_unpacked/dtb}
[ -f "$DTB" ] || { echo "error: no DTB at $DTB (run tools/unpack_bootv0.py on your stock boot.img, or set DTB=)" >&2; exit 1; }
mkdir -p out
objs=
for f in $SRCS; do
	$CC $CFLAGS -c $f -o out/${f%.*}.o
	objs="$objs out/${f%.*}.o"
done
# The image runs wherever ABL puts it with no relocation pass: only the
# header's image_size (start.o, linked at 0) may be an absolute address.
abs=$(aarch64-linux-gnu-readelf -r ${objs/out\/start.o/} | grep -c ABS64 || true)
[ "$abs" = 0 ] || { echo "error: $abs absolute relocations in C objects" >&2; exit 1; }
$CC $CFLAGS -static -T shim.ld -Wl,--build-id=none -Wl,--no-warn-rwx-segments -Wl,-N \
	$objs -o out/shim.elf
$OBJCOPY -O binary out/shim.elf out/Image
# ABL copies the file; pad to image_size so .bss/stacks exist as zeroed bytes.
size=$((16#$(aarch64-linux-gnu-nm out/shim.elf | awk '$3=="_end"{print $1}')))
truncate -s "$size" out/Image
# Ramdisk: an xnu payload (tools/mkpayload.py) if given as $1, else an empty
# newc cpio (ABL dislikes a zero-size ramdisk on some builds) -> probe.
(cd out && rm -rf rd && mkdir rd && cd rd && find . | cpio -o -H newc --quiet | gzip -9 > ../ramdisk.bin)
if [ -n "${1:-}" ]; then
	if [ "${EMBED:-0}" = 1 ]; then
		# loader.c looks behind _end first: append at the next 4K boundary
		# and grow image_size so ABL reserves room for it.
		truncate -s $(( (size + 0xfff) & ~0xfff )) out/Image
		cat "$1" >> out/Image
		python3 -c 'import os,struct,sys; f=open(sys.argv[1],"r+b"); f.seek(16); f.write(struct.pack("<Q", os.path.getsize(sys.argv[1])))' out/Image
	else
		cp "$1" out/ramdisk.bin
	fi
fi
gzip -9 -n -c out/Image > out/Image.gz
cat out/Image.gz "$DTB" > out/Image.gz-dtb
# Offsets of the LineageOS cheeseburger BoardConfig (base 0, 4K pages);
# ABL places the kernel itself, the shim runs wherever it lands.
${MKBOOTIMG:-mkbootimg} --header_version 0 --kernel out/Image.gz-dtb --ramdisk out/ramdisk.bin \
	--base 0 --kernel_offset 0x8000 --ramdisk_offset 0x1000000 --tags_offset 0x100 \
	--pagesize 4096 --os_version 10.0.0 --os_patch_level 2021-01 \
	--cmdline "console=null" -o out/shim-boot.img
# Host render of the console layout.
gcc -O1 -Wall -o out/host_test console.c host_test.c && ./out/host_test
ls -l out/Image out/Image.gz-dtb out/shim-boot.img
