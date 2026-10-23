#!/bin/bash
# Build the mainline Linux OnePlus 5 DTB (msm8998-oneplus-cheeseburger.dts),
# for shim/build.sh when no stock boot.img DTB is at hand. ABL selects it by
# qcom,msm-id <292 0x20001> and qcom,board-id <8 0 16859 23>. Prefer the
# stock DTB (tools/unpack_bootv0.py): it is what ABL normally boots.
#   tools/mkdtb-mainline.sh            -> dtb/mainline/msm8998-oneplus-cheeseburger.dtb
# Needs git, cpp and dtc (device-tree-compiler).
set -euo pipefail
cd "$(dirname "$0")/.."
src=research/linux-dts
if [ ! -d "$src/.git" ]; then
	git clone -q --depth 1 --filter=blob:none --sparse https://github.com/torvalds/linux "$src"
	git -C "$src" sparse-checkout set include/dt-bindings scripts/dtc/include-prefixes arch/arm64/boot/dts/qcom
	git -C "$src" sparse-checkout add include/uapi/linux/input-event-codes.h --skip-checks
fi
mkdir -p dtb/mainline
cpp -nostdinc -undef -D__DTS__ -x assembler-with-cpp -I "$src/include" \
	-I "$src/arch/arm64/boot/dts/qcom" -I "$src/scripts/dtc/include-prefixes" \
	"$src/arch/arm64/boot/dts/qcom/msm8998-oneplus-cheeseburger.dts" -o dtb/mainline/cheeseburger.dts.pre
dtc -q -I dts -O dtb -o dtb/mainline/msm8998-oneplus-cheeseburger.dtb dtb/mainline/cheeseburger.dts.pre
echo "linux $(git -C "$src" log -1 --format=%h) -> dtb/mainline/msm8998-oneplus-cheeseburger.dtb"
