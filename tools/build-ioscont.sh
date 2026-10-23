#!/bin/bash
# Static aarch64 build of tools/ioscont.c for the phone (runs on Android or TWRP).
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p ../shim/out
aarch64-linux-gnu-gcc -O2 -Wall -Wextra -static -o ../shim/out/ioscont ioscont.c
aarch64-linux-gnu-strip ../shim/out/ioscont
ls -l ../shim/out/ioscont
