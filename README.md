# ios-cheeseburger

**Porting Apple's iOS 16 (XNU kernel + iPhone X userland) to the OnePlus 5 (A5000, `cheeseburger`, Snapdragon 835 / MSM8998).**

This is a fork of [MakrSas/ios-topaz](https://github.com/MakrSas/ios-topaz) (iOS 16 on the Redmi Note 12 4G), which is itself a port of [dipa-studio/ios-sweet](https://github.com/dipa-studio/ios-sweet). Topaz is the right parent: its Snapdragon 685 has the same CPU pairing as the OnePlus 5's Snapdragon 835 (Cortex-A73 class "Gold" cores that have the 16K page granule XNU needs, Cortex-A53 class "Silver" cores that do not, all ARMv8.0), and topaz already solved booting XNU on the Gold cores and running the iPhone X userland on ARMv8.0.

> **Status (2026-10-23): not booted yet.** The stage 0 hardware probe is ready to run (nothing is flashed). Kernel, ADT and boot image are retargeted to the OnePlus 5 but untested on a phone. No touch, key, Wi-Fi or GPU driver exists for this board yet. See [PORT-OP5.md](PORT-OP5.md) for the hardware map, the plan and the driver work list.

> Experimental. When you later flash the iOS root it goes into `userdata` and **erases Android's user data**. Back up `persist`, `modemst1`, `modemst2`, `fsg`, `fsc` and `boot` first: they hold calibration data that cannot be downloaded anywhere.

## Stage 0: run the probe (safe, RAM only)

You need a OnePlus 5 (A5000) with an **unlocked bootloader** and a PC with `fastboot`.

```sh
fastboot boot shim/out/shim-boot.img     # nothing is written to the phone
```

The shim starts on a Silver core, hands over to Gold CPU `0x100` (as on topaz), tests the GIC, then shows four red pages for about 45 seconds each. **Take a photo of every page** and share them:

| Page | Shows | Decides |
| --- | --- | --- |
| (first screen) | Silver MIDR/MMFR0, Gold hand-off, GIC and timer interrupts, `SPEED` | Gold boot works, interrupts work |
| 1/4 cores | MIDR, MMFR0/1, ISAR0 and decoded `tg16`, `lse`, `pan`, `rcpc` for all 8 cores, plus a VERDICT | Whether XNU can run on Gold with 16K pages |
| 2/4 memory | RAM ranges and every reserved region from the DTB ABL passed | The RAM range given to XNU (`tools/mkadt.py --highmem`) |
| 3/4 PMIC | PM8998 over SPMI v3: power and volume keys (with a 20 s live view, press them), touch rails L28/L6, I2C clocks and pins | Scope of the key and touch drivers |
| 4/4 UFS | UFS host state and a read-only GPT read | The storage driver works unchanged |

Then the phone reboots to fastboot by itself (about 5 minutes in total). If the screen stays black but the phone returns to fastboot, the code ran and only the framebuffer address is wrong: say so.

## Build

```sh
sudo apt install gcc-aarch64-linux-gnu device-tree-compiler mkbootimg cpio
# DTB: preferably your stock one ...
python3 tools/unpack_bootv0.py backup/boot.img dtb/boot_unpacked
shim/build.sh                                  # probe image -> shim/out/shim-boot.img
# ... or the mainline Linux OnePlus 5 DTB
tools/mkdtb-mainline.sh
DTB=../dtb/mainline/msm8998-oneplus-cheeseburger.dtb shim/build.sh
```

Debian/Ubuntu's `mkbootimg` package lacks its `gki` module; point `MKBOOTIMG=` at AOSP's `mkbootimg.py` if it fails.

The XNU kernel builds on GitHub's macOS runners exactly as for topaz (`gh workflow run xnu.yml`, then `gh run download ... -n xnu-kernel -D xnu/out`); see [README-topaz.md](README-topaz.md). The series here is cut after patch 0082 (sweet's generic bring-up plus topaz's ARMv8.0 and Gold boot work); topaz's board drivers are kept in `xnu/patches-topaz/` for later.

## Layout

| Path | What |
| --- | --- |
| `PORT-OP5.md` | Hardware map, what changed from topaz, stages, driver work list, open questions |
| `shim/` | Boot shim (boot.img v0, `Image.gz` + appended DTB) and the probe (`probe.c`) |
| `xnu/patches/` | Kernel patches 0001-0082 applied by CI |
| `xnu/patches-topaz/` | Topaz patches 0083+ (SM6225 drivers, GPU, Wi-Fi, audio), not built |
| `tools/mkadt.py` | Apple Device Tree for the OnePlus 5 |
| `tools/unpack_bootv0.py` | Splits a stock OnePlus 5 `boot.img` into kernel, ramdisk and DTBs |
| `tools/mkdtb-mainline.sh` | Builds the mainline Linux OnePlus 5 DTB |
| `README-topaz.md`, `NOTES.md` | Topaz and sweet documentation and engineering log |

## Credits

[ios-sweet](https://github.com/dipa-studio/ios-sweet) (dipa-studio), [ios-topaz](https://github.com/MakrSas/ios-topaz) (MakrSas), HackiOS (see [CREDITS.md](CREDITS.md)), Apple's open-source XNU, and the mainline Linux MSM8998 / OnePlus 5 device trees (Jami Kettunen and others) plus the OnePlus msm8998 kernel source for the hardware details. iOS itself is Apple's and is not distributed here.
