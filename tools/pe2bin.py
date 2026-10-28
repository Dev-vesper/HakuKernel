#!/usr/bin/env python3
"""pe2bin.py — convert a linked PE image (PE32 or PE32+) into a flat binary.

Usage:
    pe2bin.py kernel.pe kernel.bin

The PE must be linked for its load address (link.exe /BASE:0x10000) with a
fixed entry point (/ENTRY:...). This script does NO relocation math — the
real linker already resolved everything. It only copies each section's raw
bytes to (RVA - ImageBase) and zero-fills BSS-style sections, then reports
the entry point address for the bootloader (ENTRY=0x...).

Both the i386 kernel (machine 0x14c, magic 0x10b) and the x86-64 image
(machine 0x8664, magic 0x20b) are supported; the only difference is where
the 32- or 64-bit ImageBase field lives in the optional header.

Exit codes: 0 = ok, 1 = usage error, 2 = image not convertible (with reason).
"""

import struct
import sys


def die(msg, code=2):
    print(f"pe2bin: error: {msg}", file=sys.stderr)
    sys.exit(code)


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(1)
    pe_path, out_path = sys.argv[1], sys.argv[2]
    with open(pe_path, "rb") as f:
        data = f.read()

    if data[0:2] != b"MZ":
        die("not a PE file (missing MZ header)")
    e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    if data[e_lfanew:e_lfanew + 4] != b"PE\0\0":
        die("not a PE file (missing PE signature)")

    coff = e_lfanew + 4
    machine, nsec, _, _, _, opt_sz, _ = struct.unpack_from("<HHIIIHH", data, coff)
    # PE32 (0x14C) is the 32-bit kernel; PE32+ (0x8664) is the x86-64 image.
    # Both are flat-loaded the same way: section RVAs are offsets from the
    # link-time base, and the bootloader puts the image there.
    if machine == 0x8664:
        pe32plus = True
    elif machine == 0x14C:
        pe32plus = False
    else:
        die(f"unsupported machine {machine:#06x} "
            "(expected 0x14c i386 or 0x8664 x86-64)")
    opt = coff + 20
    magic = struct.unpack_from("<H", data, opt)[0]
    if pe32plus:
        if magic != 0x20B:
            die(f"x86-64 image has optional-header magic {magic:#05x}, "
                "expected 0x20b (PE32+)")
        entry_rva = struct.unpack_from("<I", data, opt + 16)[0]
        image_base = struct.unpack_from("<Q", data, opt + 24)[0]
    else:
        if magic != 0x10B:
            die("not a PE32 image (expected optional magic 0x10B)")
        entry_rva = struct.unpack_from("<I", data, opt + 16)[0]
        image_base = struct.unpack_from("<I", data, opt + 28)[0]

    sectab = opt + opt_sz
    sections = []
    for i in range(nsec):
        name_b, vsize, vaddr, raw_sz, raw_ptr, _, _, _, _, chars = \
            struct.unpack_from("<8sIIIIIIHHI", data, sectab + 40 * i)
        name = name_b.split(b"\x00")[0].decode("ascii", "replace")
        if name.startswith((".debug", ".drectve", ".pdata", ".edata")):
            continue  # linker/debug metadata, not needed at runtime
        raw = data[raw_ptr:raw_ptr + raw_sz] if raw_sz else b""
        size = max(vsize, len(raw))
        sections.append((name, vaddr, raw, size))

    if not sections:
        die("no loadable sections found")

    total = 0
    for _, vaddr, _, size in sections:
        total = max(total, vaddr + size)
    if total <= 0:
        die("empty image")
    out = bytearray(total)
    for name, vaddr, raw, size in sections:
        start = vaddr  # file offsets are relative to ImageBase
        out[start:start + len(raw)] = raw
        # the rest stays zeroed (BSS / alignment gaps)

    if image_base % 0x10000:
        die(f"image base {image_base:#x} is not 64 KiB aligned; "
            "link with /BASE:0x10000")
    entry = image_base + entry_rva

    with open(out_path, "wb") as f:
        f.write(out)

    print(f"pe2bin: {pe_path} -> {out_path} ({len(out)} bytes, base={image_base:#x})")
    for name, vaddr, raw, size in sections:
        print(f"  {image_base + vaddr:#10x}  {size:6d}  {name}")
    print(f"ENTRY={entry:#x}")


if __name__ == "__main__":
    main()
