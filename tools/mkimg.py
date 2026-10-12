#!/usr/bin/env python3
"""mkimg.py — assemble the bootable 1.44MB floppy image.

Usage:
    mkimg.py stage1.bin stage2.bin kernel.bin haku.img stage1.asm stage2.asm

Layout: [stage1: 1 sector][stage2: STAGE2_SECTORS][kernel: KERNEL_SECTORS].
Checks: stage1 is 512 bytes with 0xAA55; stage2 fits its sector budget;
kernel fits KERNEL_SECTORS (parsed from stage2.asm); sector math is
cross-checked (1 + stage2_sectors + 1 == kernel_first_sector).
Then concatenates (stage2 padded to whole sectors) and zero-pads to
1474560 bytes.
"""

import re
import sys

FLOPPY_SIZE = 1474560


def die(msg):
    print(f"mkimg: error: {msg}", file=sys.stderr)
    sys.exit(1)


def read_equ(path, name):
    with open(path, "rb") as f:
        src = f.read().decode("ascii", "replace")
    m = re.search(rf"{name}\s+equ\s+(\d+)", src)
    if not m:
        die(f"{name} not found in {path}")
    return int(m.group(1))


def main():
    if len(sys.argv) != 7:
        print(__doc__)
        sys.exit(1)
    s1_p, s2_p, kern_p, img_p, s1_asm, s2_asm = sys.argv[1:7]

    with open(s1_p, "rb") as f:
        s1 = f.read()
    if len(s1) != 512:
        die(f"stage1 is {len(s1)} bytes, must be exactly 512")
    if s1[510] != 0x55 or s1[511] != 0xAA:
        die("boot signature 0xAA55 missing")

    stage2_sectors = read_equ(s1_asm, "STAGE2_SECTORS")
    kernel_sectors = read_equ(s2_asm, "KERNEL_SECTORS")
    kernel_first = read_equ(s2_asm, "KERNEL_FIRST_SECTOR")
    if 1 + stage2_sectors + 1 != kernel_first:
        die(f"sector math broken: 1 + {stage2_sectors} + 1 != "
            f"KERNEL_FIRST_SECTOR ({kernel_first}); fix the .asm files")

    with open(s2_p, "rb") as f:
        s2 = f.read()
    if len(s2) > stage2_sectors * 512:
        die(f"stage2 is {len(s2)} bytes > budget {stage2_sectors * 512}; "
            "raise STAGE2_SECTORS in stage1.asm (and KERNEL_FIRST_SECTOR)")

    with open(kern_p, "rb") as f:
        kern = f.read()
    if len(kern) > kernel_sectors * 512:
        die(f"kernel.bin is {len(kern)} bytes > budget "
            f"{kernel_sectors * 512} (raise KERNEL_SECTORS in stage2.asm)")

    img = bytearray(FLOPPY_SIZE)
    img[0:512] = s1
    img[512:512 + len(s2)] = s2
    # kernel_first is a 1-based CHS sector number; file offsets are 0-based.
    k_off = 512 * (kernel_first - 1)
    img[k_off:k_off + len(kern)] = kern
    with open(img_p, "wb") as f:
        f.write(img)

    print(f"mkimg: {img_p} ready (stage1 512 B + stage2 {len(s2)} B + "
          f"kernel {len(kern)} B = {(len(kern) + 511) // 512}/{kernel_sectors} sectors)")


if __name__ == "__main__":
    main()
