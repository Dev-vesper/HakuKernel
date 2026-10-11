#!/usr/bin/env python3
"""mkimg.py — assemble the bootable 1.44MB floppy image.

Usage:
    mkimg.py boot.bin kernel.bin myos.img boot.asm

Checks: boot sector is 512 bytes with 0xAA55 signature; kernel fits the
KERNEL_SECTORS budget parsed from boot.asm and stays within one floppy
track (17 sectors — the bootloader does a single-track read).
Then concatenates boot + kernel and zero-pads to 1474560 bytes.
"""

import re
import sys

FLOPPY_SIZE = 1474560
SINGLE_TRACK_SECTORS = 17


def die(msg):
    print(f"mkimg: error: {msg}", file=sys.stderr)
    sys.exit(1)


def main():
    if len(sys.argv) != 5:
        print(__doc__)
        sys.exit(1)
    boot_p, kern_p, img_p, asm_p = sys.argv[1:5]

    with open(boot_p, "rb") as f:
        boot = f.read()
    if len(boot) != 512:
        die(f"boot.bin is {len(boot)} bytes, must be exactly 512")
    if boot[510] != 0x55 or boot[511] != 0xAA:
        die("boot signature 0xAA55 missing")

    with open(asm_p, "rb") as f:
        src = f.read().decode("ascii", "replace")
    m = re.search(r"KERNEL_SECTORS\s+equ\s+(\d+)", src)
    if not m:
        die("KERNEL_SECTORS not found in boot.asm")
    budget = int(m.group(1))
    if budget > SINGLE_TRACK_SECTORS:
        die("KERNEL_SECTORS > 17: boot.asm does a single-track read; "
            "extend it to a CHS loop first")

    with open(kern_p, "rb") as f:
        kern = f.read()
    if len(kern) > budget * 512:
        die(f"kernel.bin is {len(kern)} bytes > budget {budget * 512} "
            "(raise KERNEL_SECTORS in boot.asm)")
    nsec = (len(kern) + 511) // 512
    if nsec > SINGLE_TRACK_SECTORS:
        die(f"kernel.bin is {nsec} sectors, beyond one floppy track (17); "
            "extend boot.asm with a multi-track CHS loop (or shrink the kernel)")

    img = bytearray(FLOPPY_SIZE)
    img[0:512] = boot
    img[512:512 + len(kern)] = kern
    with open(img_p, "wb") as f:
        f.write(img)

    print(f"mkimg: {img_p} ready (boot 512 B + kernel {len(kern)} B = "
          f"{nsec}/{budget} sectors)")


if __name__ == "__main__":
    main()
