#!/usr/bin/env python3
"""Convert a fixed-base PE32 image into the ELF32 layout Haku loads."""

import struct
import sys

PT_LOAD = 1
PF_X = 1
PF_W = 2
PF_R = 4

IMAGE_SCN_MEM_DISCARDABLE = 0x02000000
IMAGE_SCN_MEM_EXECUTE = 0x20000000
IMAGE_SCN_MEM_READ = 0x40000000
IMAGE_SCN_MEM_WRITE = 0x80000000


def align_up(value, alignment):
    return (value + alignment - 1) & ~(alignment - 1)


def unpack_from(fmt, data, offset):
    try:
        return struct.unpack_from(fmt, data, offset)
    except struct.error:
        sys.exit("pe2elf: truncated PE image")


def main():
    if len(sys.argv) != 3:
        sys.exit("usage: pe2elf.py input.exe output.elf")
    source_path, output_path = sys.argv[1], sys.argv[2]
    with open(source_path, "rb") as source:
        image = source.read()

    if image[:2] != b"MZ":
        sys.exit("pe2elf: missing MZ header")
    pe_offset = unpack_from("<I", image, 0x3C)[0]
    if image[pe_offset:pe_offset + 4] != b"PE\0\0":
        sys.exit("pe2elf: missing PE signature")
    coff = pe_offset + 4
    section_count = unpack_from("<H", image, coff + 2)[0]
    optional_size = unpack_from("<H", image, coff + 16)[0]
    optional = coff + 20
    if unpack_from("<H", image, optional)[0] != 0x10B:
        sys.exit("pe2elf: not a PE32 image")
    entry_rva = unpack_from("<I", image, optional + 16)[0]
    image_base = unpack_from("<I", image, optional + 28)[0]

    segments = []
    table = optional + optional_size
    for index in range(section_count):
        header = table + index * 40
        name = image[header:header + 8].rstrip(b"\0").decode("ascii", "replace")
        virtual_size, virtual_address, raw_size, raw_pointer = \
            unpack_from("<IIII", image, header + 8)
        characteristics = unpack_from("<I", image, header + 36)[0]
        if characteristics & IMAGE_SCN_MEM_DISCARDABLE:
            continue
        if not characteristics & IMAGE_SCN_MEM_READ:
            continue
        if virtual_size == 0 and raw_size == 0:
            continue
        if raw_size and raw_pointer + raw_size > len(image):
            sys.exit(f"pe2elf: section {name} is outside the image")
        memory_size = max(virtual_size, raw_size)
        virtual_address += image_base
        if virtual_address & 0xFFF:
            sys.exit("pe2elf: sections must be page aligned (/ALIGN:4096)")
        flags = PF_R
        if characteristics & IMAGE_SCN_MEM_WRITE:
            flags |= PF_W
        if characteristics & IMAGE_SCN_MEM_EXECUTE:
            flags |= PF_X
        if (flags & (PF_W | PF_X)) == (PF_W | PF_X):
            sys.exit(f"pe2elf: section {name} is writable and executable")
        segments.append({
            "name": name,
            "vaddr": virtual_address,
            "filesz": raw_size,
            "memsz": memory_size,
            "flags": flags,
            "data": image[raw_pointer:raw_pointer + raw_size],
        })

    segments.sort(key=lambda segment: segment["vaddr"])
    if not segments:
        sys.exit("pe2elf: no loadable sections")
    for index in range(1, len(segments)):
        previous = segments[index - 1]
        previous_end = align_up(previous["vaddr"] + previous["memsz"], 0x1000)
        if previous_end > segments[index]["vaddr"]:
            sys.exit("pe2elf: section pages overlap")

    entry = image_base + entry_rva
    if not any(segment["flags"] & PF_X and
               segment["vaddr"] <= entry < segment["vaddr"] + segment["memsz"]
               for segment in segments):
        sys.exit("pe2elf: entry point is not inside an executable section")

    file_offset = 0x1000
    for segment in segments:
        file_offset = align_up(file_offset, 0x1000)
        if file_offset & 0xFFF:
            sys.exit("pe2elf: internal alignment failure")
        segment["offset"] = file_offset
        file_offset += segment["filesz"]
    output = bytearray(max(align_up(file_offset, 0x1000), 0x1000))

    ident = bytes([0x7F]) + b"ELF" + bytes([1, 1, 1, 0]) + bytes(8)
    header = struct.pack(
        "<16sHHIIIIIHHHHHH", ident, 2, 3, 1, entry, 52, 0, 0,
        52, 32, len(segments), 0, 0, 0)
    output[:52] = header
    for index, segment in enumerate(segments):
        phdr = struct.pack(
            "<IIIIIIII", PT_LOAD, segment["offset"], segment["vaddr"],
            segment["vaddr"], segment["filesz"], segment["memsz"],
            segment["flags"], 0x1000)
        output[52 + index * 32:52 + (index + 1) * 32] = phdr
        output[segment["offset"]:segment["offset"] + segment["filesz"]] = \
            segment["data"]

    with open(output_path, "wb") as destination:
        destination.write(output)
    print(f"ELF: entry=0x{entry:08X} segments={len(segments)}")


if __name__ == "__main__":
    main()
