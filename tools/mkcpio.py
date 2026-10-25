#!/usr/bin/env python3
"""Create a cpio "newc" initramfs archive from a list of files.

Usage: mkcpio.py OUTPUT.cpio [--dir NAME]... [NAME=PATH]...

Every NAME=PATH pair becomes an archive entry called NAME holding the
contents of PATH, so the kernel can unpack a userland image into its RAM
filesystem at boot. The format is the SVR4 "newc" one Linux and every
initramfs generator emit, which keeps the archive inspectable with the
usual host tools (`cpio -it < archive`).
"""

import pathlib
import sys

HEADER_SIZE = 110
MAGIC = b"070701"

MODE_FILE = 0o100644
MODE_DIR = 0o040755


def pad4(data):
    return data + b"\0" * ((4 - len(data) % 4) % 4)


def header(ino, mode, size, namesize):
    fields = [
        ino, mode, 0, 0, 1, 0, size, 0, 0, 0, 0, namesize, 0,
    ]
    text = MAGIC + b"".join(b"%08X" % (value & 0xFFFFFFFF) for value in fields)
    if len(text) != HEADER_SIZE:
        raise ValueError("internal error: header length")
    return text


def entry(ino, name, mode, payload=b""):
    encoded = name.encode("ascii") + b"\0"
    return (
        pad4(header(ino, mode, len(payload), len(encoded)) + encoded)
        + pad4(payload)
    )


def main(argv):
    if len(argv) < 2:
        sys.exit(__doc__)
    output = pathlib.Path(argv[1])
    files = []
    directories = []
    for argument in argv[2:]:
        if argument.startswith("--dir="):
            directories.append(argument[len("--dir="):])
            continue
        if "=" not in argument:
            sys.exit(f"mkcpio: expected NAME=PATH, got {argument!r}")
        name, path = argument.split("=", 1)
        files.append((name, pathlib.Path(path)))

    archive = bytearray()
    ino = 1
    for name in directories:
        archive += entry(ino, name, MODE_DIR)
        ino += 1
    for name, path in files:
        try:
            payload = path.read_bytes()
        except OSError as error:
            sys.exit(f"mkcpio: cannot read {path}: {error}")
        archive += entry(ino, name, MODE_FILE, payload)
        ino += 1
    # Trailer: a header whose name is TRAILER!!! and no data.
    archive += pad4(header(0, 0, 0, len(b"TRAILER!!!\0")) + b"TRAILER!!!\0")
    # cpio pads the archive to a 512-byte block; the kernel stops at the
    # trailer, but matching the convention keeps other tools happy.
    if len(archive) % 512:
        archive += b"\0" * (512 - len(archive) % 512)

    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(archive)
    total = sum(len(p.read_bytes()) for _, p in files)
    print(f"mkcpio: {output} ready ({len(archive)} bytes, "
          f"{len(files)} files, {total} bytes of payload)")


if __name__ == "__main__":
    main(sys.argv)
