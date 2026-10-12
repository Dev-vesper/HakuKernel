#!/usr/bin/env python3
"""Create a minimal FAT12 superfloppy image with one root-directory file."""

import pathlib
import struct
import sys

SECTOR_SIZE = 512
TOTAL_SECTORS = 2880
SECTORS_PER_CLUSTER = 1
RESERVED_SECTORS = 1
FAT_COUNT = 2
ROOT_ENTRIES = 224
SECTORS_PER_FAT = 9
ROOT_SECTORS = ROOT_ENTRIES * 32 // SECTOR_SIZE
FAT_START = RESERVED_SECTORS
ROOT_START = FAT_START + FAT_COUNT * SECTORS_PER_FAT
DATA_START = ROOT_START + ROOT_SECTORS
CLUSTER_COUNT = TOTAL_SECTORS - DATA_START


def fat12_set(fat, cluster, value):
    offset = cluster + cluster // 2
    value &= 0x0FFF
    if cluster & 1:
        fat[offset] = (fat[offset] & 0x0F) | ((value << 4) & 0xF0)
        fat[offset + 1] = (value >> 4) & 0xFF
    else:
        fat[offset] = value & 0xFF
        fat[offset + 1] = (fat[offset + 1] & 0xF0) | ((value >> 8) & 0x0F)


def main():
    if len(sys.argv) != 3:
        sys.exit("usage: mkfat12.py input.elf output.img")

    elf_path = pathlib.Path(sys.argv[1])
    image_path = pathlib.Path(sys.argv[2])
    payload = elf_path.read_bytes()
    if not payload:
        sys.exit("mkfat12: refusing to create an empty program file")

    cluster_count = (len(payload) + SECTOR_SIZE - 1) // SECTOR_SIZE
    if cluster_count > CLUSTER_COUNT:
        sys.exit("mkfat12: ELF does not fit on the FAT12 volume")

    image = bytearray(TOTAL_SECTORS * SECTOR_SIZE)
    boot = bytearray(SECTOR_SIZE)
    boot[0:3] = b"\xEB\x3C\x90"
    boot[3:11] = b"HAKUKERN"
    struct.pack_into("<HBHBHHBHHHII", boot, 11,
                     SECTOR_SIZE, SECTORS_PER_CLUSTER, RESERVED_SECTORS,
                     FAT_COUNT, ROOT_ENTRIES, TOTAL_SECTORS, 0xF0,
                     SECTORS_PER_FAT, 18, 2, 0, 0)
    boot[36:39] = b"\x00\x00\x00"
    boot[39:43] = b"HAKU"
    boot[43:54] = b"FAT12 VOL  "
    boot[54:62] = b"FAT12   "
    boot[510:512] = b"\x55\xAA"
    image[:SECTOR_SIZE] = boot

    fat = bytearray(SECTORS_PER_FAT * SECTOR_SIZE)
    fat[0:3] = b"\xF0\xFF\xFF"
    for index in range(cluster_count):
        current = index + 2
        following = 0x0FFF if index == cluster_count - 1 else current + 1
        fat12_set(fat, current, following)
        source = index * SECTOR_SIZE
        destination = (DATA_START + index) * SECTOR_SIZE
        image[destination:destination + SECTOR_SIZE] = \
            payload[source:source + SECTOR_SIZE].ljust(SECTOR_SIZE, b"\0")

    for index in range(FAT_COUNT):
        start = (FAT_START + index * SECTORS_PER_FAT) * SECTOR_SIZE
        image[start:start + len(fat)] = fat

    root = ROOT_START * SECTOR_SIZE
    image[root:root + 11] = b"HAKU    ELF"
    image[root + 11] = 0x20
    struct.pack_into("<H", image, root + 26, 2)
    struct.pack_into("<I", image, root + 28, len(payload))

    image_path.write_bytes(image)
    print(f"mkfat12: {image_path} ready ({len(payload)} byte HAKU.ELF, "
          f"{cluster_count} clusters, {TOTAL_SECTORS} sectors)")


if __name__ == "__main__":
    main()
