#!/usr/bin/env python3
"""Unpack a boot.img v0/v1 whose kernel carries appended DTBs (Image.gz-dtb),
as on the OnePlus 5 (cheeseburger). Writes OUT/kernel, OUT/ramdisk, OUT/dtb
(all appended DTBs, concatenated, what shim/build.sh appends again) and
OUT/dtb.N, and prints each DTB's model, msm-id and board-id so you can see
which one ABL will pick.

Usage: unpack_bootv0.py backup/boot.img dtb/boot_unpacked
"""
import os, struct, sys, zlib

FDT_MAGIC = b'\xd0\x0d\xfe\xed'


def fdt_props(blob, want):
    """Root-node properties named in `want` (raw bytes), from a flattened DT."""
    off_struct, off_strings = struct.unpack_from('>II', blob, 8)
    p, depth, out = off_struct, 0, {}
    while p + 4 <= len(blob):
        tok, = struct.unpack_from('>I', blob, p)
        p += 4
        if tok == 1:
            end = blob.index(b'\0', p)
            p = (end + 4) & ~3
            depth += 1
        elif tok == 2:
            depth -= 1
            if depth == 0:
                break
        elif tok == 3:
            ln, nameoff = struct.unpack_from('>II', blob, p)
            name = blob[off_strings + nameoff:blob.index(b'\0', off_strings + nameoff)].decode()
            if depth == 1 and name in want:
                out[name] = blob[p + 8:p + 8 + ln]
            p = (p + 8 + ln + 3) & ~3
        elif tok == 4:
            continue
        else:
            break
    return out


def cells(b):
    return ' '.join(str(v) for v in struct.unpack('>%dI' % (len(b) // 4), b))


def main(img_path, out_dir):
    b = open(img_path, 'rb').read()
    if b[:8] != b'ANDROID!':
        raise SystemExit('not an Android boot image')
    ks, ka, rs, ra, ss, sa, ta, ps, hv, osv = struct.unpack_from('<10I', b, 8)
    if hv > 1:
        raise SystemExit(f'header v{hv}: use unpack_bootv2.py')
    cmdline = b[64:64 + 512].split(b'\0')[0].decode(errors='replace')
    patch = osv & 0x7ff
    print(f'header v{hv}  page {ps}  kernel {ks} @ {ka:#x}  ramdisk {rs} @ {ra:#x}  tags @ {ta:#x}')
    print(f'os_version {osv >> 25 & 0x7f}.{osv >> 18 & 0x7f}.{osv >> 11 & 0x7f}  '
          f'patch {2000 + (patch >> 4)}-{patch & 0xf:02d}')
    print(f'cmdline: {cmdline}')

    kern = b[ps:ps + ks]
    rd_off = ps + ((ks + ps - 1) // ps) * ps
    ramdisk = b[rd_off:rd_off + rs]
    if kern[:2] == b'\x1f\x8b':
        d = zlib.decompressobj(16 + zlib.MAX_WBITS)
        image = d.decompress(kern)
        tail = d.unused_data
        print(f'kernel: gzip, {len(image)} bytes decompressed, {len(tail)} bytes appended')
    else:
        # Raw Image: appended DTBs start after the Image (header image_size at 16)
        size = struct.unpack_from('<Q', kern, 16)[0] if kern[0x38:0x3c] == b'ARMd' else 0
        first = kern.find(FDT_MAGIC, size)
        image, tail = (kern, b'') if first < 0 else (kern[:first], kern[first:])
        print(f'kernel: raw, {len(image)} bytes, {len(tail)} bytes appended')

    os.makedirs(out_dir, exist_ok=True)
    open(os.path.join(out_dir, 'kernel'), 'wb').write(image)
    open(os.path.join(out_dir, 'ramdisk'), 'wb').write(ramdisk)

    dtbs, p = [], 0
    while True:
        p = tail.find(FDT_MAGIC, p)
        if p < 0:
            break
        total, = struct.unpack_from('>I', tail, p + 4)
        dtbs.append(tail[p:p + total])
        p += total
    if not dtbs:
        raise SystemExit('no appended DTB found')
    open(os.path.join(out_dir, 'dtb'), 'wb').write(b''.join(dtbs))
    for i, blob in enumerate(dtbs):
        open(os.path.join(out_dir, f'dtb.{i}'), 'wb').write(blob)
        pr = fdt_props(blob, {'model', 'qcom,msm-id', 'qcom,board-id', 'qcom,pmic-id'})
        model = pr.get('model', b'?').rstrip(b'\0').decode(errors='replace')
        print(f'dtb.{i}: {len(blob)} bytes  "{model}"  msm-id <{cells(pr.get("qcom,msm-id", b""))}>  '
              f'board-id <{cells(pr.get("qcom,board-id", b""))}>')
    print(f'-> {out_dir}/dtb ({len(dtbs)} DTBs) for shim/build.sh')


if __name__ == '__main__':
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    main(sys.argv[1], sys.argv[2])
