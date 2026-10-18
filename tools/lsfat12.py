#!/usr/bin/env python3
"""Inspect a FAT12 superfloppy image (read-only host-side view).

Usage:
    lsfat12.py IMAGE.img                 list root directory + FAT summary
    lsfat12.py IMAGE.img --chains        also print each file's cluster chain
    lsfat12.py IMAGE.img --check         verify layout the way the Haku kernel
                                         requires it; exit 2 on any damage
    lsfat12.py IMAGE.img --cat NAME.EXT  write the file's bytes to stdout
    lsfat12.py IMAGE.img --dump NAME.EXT OUT.BIN
                                         extract the file to OUT.BIN

Never modifies the image. --check enforces the mount invariants from
kernel/fat12.c (sector size, BPB sanity, 0x55AA signature, mirrored FATs,
exact cluster chains, no orphaned clusters) so a passing check means the
kernel will mount the volume and every file reads back whole.
"""

import struct
import sys

EOC_MIN = 0xFF8
BAD = 0xFF7
SECTOR = 512


class ImageError(Exception):
    """The image itself is damaged or unreadable (not a usage error)."""


def eprint(*args):
    print(*args, file=sys.stderr)


def parse_bpb(data):
    """Unpack the BIOS Parameter Block; raise ImageError if too small."""
    if len(data) < SECTOR:
        raise ImageError("image smaller than one sector")
    boot = data[:SECTOR]
    (bps, spc, reserved, fats, roots, total16, _media, fat_sectors,
     _spt, _heads, _hidden, total32) = struct.unpack_from(
        "<HBHBHHBHHHII", boot, 11)
    total = total16 if total16 else total32
    signature = boot[510:512]
    fat_start = reserved
    root_start = fat_start + fats * fat_sectors
    root_sectors = (roots * 32 + SECTOR - 1) // SECTOR
    data_start = root_start + root_sectors
    clusters = total - data_start
    return {
        "bps": bps, "spc": spc, "reserved": reserved, "fats": fats,
        "roots": roots, "total": total, "fat_sectors": fat_sectors,
        "signature": signature, "fat_start": fat_start,
        "root_start": root_start, "root_sectors": root_sectors,
        "data_start": data_start, "clusters": clusters,
        "cluster_bytes": spc * bps,
    }


def check_bpb(bpb, image_len):
    """Kernel mount invariants (kernel/fat12.c fat12_mount)."""
    problems = []
    if bpb["bps"] != SECTOR:
        problems.append(f"bytes/sector={bpb['bps']}, want 512")
    if bpb["spc"] not in (1, 2, 4, 8):
        problems.append(f"sectors/cluster={bpb['spc']}, want 1/2/4/8")
    if bpb["reserved"] != 1:
        problems.append(f"reserved={bpb['reserved']}, want 1")
    if bpb["fats"] not in (1, 2):
        problems.append(f"fat-count={bpb['fats']}, want 1..2")
    if not bpb["roots"]:
        problems.append("no root entries")
    if not bpb["fat_sectors"]:
        problems.append("no FAT sectors")
    if not bpb["total"]:
        problems.append("zero total sectors")
    if image_len != bpb["total"] * SECTOR:
        problems.append(f"file is {image_len} bytes, BPB says "
                        f"{bpb['total'] * SECTOR}")
    if bpb["signature"] != b"\x55\xAA":
        problems.append("missing 0x55AA boot signature")
    if bpb["data_start"] >= bpb["total"]:
        problems.append("data area starts past end of volume")
    if not 1 <= bpb["clusters"] < 4085:
        problems.append(f"cluster_count={bpb['clusters']}, want 1..4084")
    fat_bytes = bpb["fat_sectors"] * SECTOR
    need_bytes = ((bpb["clusters"] + 2) * 3 + 1) // 2
    if need_bytes > fat_bytes:
        problems.append("FAT too small for its cluster count")
    return problems


def fat12_get(fat, cluster):
    """Decode one 12-bit entry; raise ImageError when out of the table."""
    offset = cluster + cluster // 2
    if offset + 1 >= len(fat):
        raise ImageError(f"cluster {cluster} outside FAT table")
    raw = fat[offset] | (fat[offset + 1] << 8)
    if cluster & 1:
        return raw >> 4
    return raw & 0x0FFF


def walk_chain(fat, first, needed, nclusters):
    """Follow a chain; return (clusters, error-or-None). Always terminates."""
    if needed == 0:
        if first == 0:
            return [], None
        return [], "zero-size file owns cluster %d" % first
    chain = []
    seen = set()
    current = first
    while True:
        if current >= EOC_MIN:
            break
        if current == BAD:
            return chain, "chain hits BAD cluster"
        if not 2 <= current <= nclusters + 1:
            return chain, "cluster %d out of range" % current
        if current in seen:
            return chain, "cyclic chain at cluster %d" % current
        seen.add(current)
        chain.append(current)
        if len(chain) > nclusters + 1:
            return chain, "chain longer than volume"
        current = fat12_get(fat, current)
    if len(chain) < needed:
        return chain, "chain truncated (%d clusters for %d bytes)" % (
            len(chain), needed)
    if len(chain) > needed:
        return chain, "chain longer than file size"
    return chain, None


def parse_root(data, bpb):
    """Split root entries into live files, dirs, labels, deleted."""
    files, deleted = [], []
    base = bpb["root_start"] * SECTOR
    for index in range(bpb["roots"]):
        entry = data[base + index * 32: base + (index + 1) * 32]
        if entry[0] == 0x00:
            break  # end of directory
        if entry[0] == 0xE5:
            deleted.append(entry)
            continue
        attr = entry[11]
        if attr == 0x0F:
            continue  # long-name fragment; kernel ignores these too
        name = entry[0:11]
        first = struct.unpack_from("<H", entry, 26)[0]
        size = struct.unpack_from("<I", entry, 28)[0]
        if attr & 0x08 and not attr & 0x10:
            continue  # volume label, no data
        files.append({"raw": name, "attr": attr, "first": first,
                      "size": size, "index": index})
    return files, deleted


def show_name(raw):
    base = raw[0:8].decode("ascii", "replace").rstrip()
    ext = raw[8:11].decode("ascii", "replace").rstrip()
    return base + ("." + ext if ext else "")


def show_attr(attr):
    letters = [(0x01, "R"), (0x02, "H"), (0x04, "S"),
               (0x08, "V"), (0x10, "D"), (0x20, "A")]
    text = "".join(letter for bit, letter in letters if attr & bit)
    return text if text else "-"


def parse_wanted(text):
    """Accept NAME.EXT (case-insensitive) into raw 8.3 bytes."""
    clean = text.strip().upper()
    if "." in clean:
        base, ext = clean.split(".", 1)
    else:
        base, ext = clean, ""
    if not base or len(base) > 8 or len(ext) > 3:
        sys.exit(f"bad 8.3 name: {text!r}")
    try:
        return (base.ljust(8) + ext.ljust(3)).encode("ascii")
    except UnicodeEncodeError:
        sys.exit(f"bad 8.3 name: {text!r}")


def read_file_bytes(data, bpb, fat, entry):
    """Collect a file's bytes; raise ImageError on any chain damage."""
    needed = 0 if entry["size"] == 0 else (
        (entry["size"] + bpb["cluster_bytes"] - 1) // bpb["cluster_bytes"])
    chain, error = walk_chain(fat, entry["first"], needed, bpb["clusters"])
    if error:
        raise ImageError(f"{show_name(entry['raw'])}: {error}")
    out = bytearray()
    for cluster in chain:
        lba = bpb["data_start"] + (cluster - 2) * bpb["spc"]
        out += data[lba * SECTOR:(lba + bpb["spc"]) * SECTOR]
    return bytes(out[:entry["size"]])


def walk_all(fat, first, nclusters):
    """Follow a whole chain to EOF; error only on damaged links/cycles."""
    chain = []
    seen = set()
    current = first
    while 2 <= current <= nclusters + 1:
        if current in seen:
            return chain, "cyclic chain at cluster %d" % current
        seen.add(current)
        chain.append(current)
        if len(chain) > nclusters + 1:
            return chain, "chain longer than volume"
        value = fat12_get(fat, current)
        if value == BAD:
            return chain, "chain hits BAD cluster"
        if value >= EOC_MIN:
            return chain, None
        current = value
    if not chain:
        return chain, "directory starts at cluster %d" % first
    return chain, None


def read_dir_entries(data, bpb, fat, first):
    """Return (entries, chain, error) for the root region or one directory."""
    entries = []
    if first == 0:
        base = bpb["root_start"] * SECTOR
        for index in range(bpb["roots"]):
            entry = data[base + index * 32: base + (index + 1) * 32]
            if len(entry) < 32 or entry[0] == 0x00:
                break
            entries.append(entry)
        return entries, [], None
    chain, error = walk_all(fat, first, bpb["clusters"])
    if error:
        return entries, chain, error
    for cluster in chain:
        lba = bpb["data_start"] + (cluster - 2) * bpb["spc"]
        block = data[lba * SECTOR:(lba + bpb["spc"]) * SECTOR]
        for offset in range(0, len(block), 32):
            entry = block[offset:offset + 32]
            if len(entry) < 32 or entry[0] == 0x00:
                return entries, chain, None
            entries.append(entry)
    return entries, chain, None


def summarize_dir(data, bpb, fat, first, prefix, held, lines, problems,
                  visited):
    """Recursively census one directory's files and subdirectories."""
    if first == 0:
        problems.append(f"{prefix.rstrip('/')}: directory has no cluster")
        return
    entries, chain, error = read_dir_entries(data, bpb, fat, first)
    for cluster in chain:
        held.setdefault(cluster, prefix.rstrip("/"))
    if error:
        problems.append(f"{prefix.rstrip('/')}: {error}")
        return
    if first in visited:
        problems.append(f"{prefix.rstrip('/')}: directory cycle")
        return
    visited.add(first)
    for entry in entries:
        if entry[0] == 0xE5:
            continue
        attr = entry[11]
        if attr == 0x0F:
            continue  # long-name fragment
        if attr & 0x08 and not attr & 0x10:
            continue  # volume label
        raw = entry[0:11]
        name = show_name(raw)
        if attr & 0x10 and name in (".", ".."):
            continue  # structural self/parent links
        first_cluster = struct.unpack_from("<H", entry, 26)[0]
        size = struct.unpack_from("<I", entry, 28)[0]
        full = prefix + name
        record = {"raw": raw, "attr": attr, "first": first_cluster,
                  "size": size, "index": 0}
        if attr & 0x10:
            summarize_dir(data, bpb, fat, first_cluster, full + "/", held,
                          lines, problems, visited)
            lines.append((record, None, "DIR"))
            continue
        needed = 0 if size == 0 else (
            (size + bpb["cluster_bytes"] - 1) // bpb["cluster_bytes"])
        file_chain, file_error = walk_chain(fat, first_cluster, needed,
                                            bpb["clusters"])
        for cluster in file_chain:
            held.setdefault(cluster, full)
        lines.append((record, file_chain, file_error if file_error else "OK"))
        if file_error:
            problems.append(f"{full}: {file_error}")


def summarize(data, bpb, fat, files, deleted):
    """Census clusters (recursing into subdirectories); (lines, problems, held)."""
    lines = []
    held = {}
    problems = []
    visited = set()
    for entry in files:
        name = show_name(entry["raw"])
        if entry["attr"] & 0x10:
            summarize_dir(data, bpb, fat, entry["first"], name + "/", held,
                          lines, problems, visited)
            lines.append((entry, None, "DIR"))
            continue
        needed = 0 if entry["size"] == 0 else (
            (entry["size"] + bpb["cluster_bytes"] - 1)
            // bpb["cluster_bytes"])
        chain, error = walk_chain(fat, entry["first"], needed,
                                  bpb["clusters"])
        for cluster in chain:
            held.setdefault(cluster, name)
        lines.append((entry, chain, error if error else "OK"))
        if error:
            problems.append(f"{name}: {error}")
    return lines, problems, held


def main(argv):
    if not argv or "--help" in argv or "-h" in argv:
        print(__doc__.strip())
        return 0
    # Order-independent arguments: flags anywhere, image anywhere.
    rest = list(argv)
    action = None
    if "--cat" in rest:
        i = rest.index("--cat")
        if len(rest) < i + 2:
            eprint("lsfat12: --cat needs a NAME.EXT argument")
            return 1
        action = ("cat", rest[i + 1], None)
        del rest[i:i + 2]
    elif "--dump" in rest:
        i = rest.index("--dump")
        if len(rest) < i + 3:
            eprint("lsfat12: --dump needs NAME.EXT and OUT arguments")
            return 1
        action = ("dump", rest[i + 1], rest[i + 2])
        del rest[i:i + 3]
    flags = [a for a in rest if a.startswith("-")]
    positionals = [a for a in rest if not a.startswith("-")]
    known = {"--chains", "--check"}
    for flag in flags:
        if flag not in known:
            eprint(f"lsfat12: unknown flag: {flag}")
            print(__doc__.strip())
            return 1
    if not positionals:
        eprint("lsfat12: need an IMAGE.img path")
        return 1
    if len(positionals) > 1:
        eprint(f"lsfat12: unexpected argument: {positionals[1]}")
        return 1
    image_path = positionals[0]
    try:
        with open(image_path, "rb") as handle:
            data = handle.read()
    except OSError as exc:
        eprint(f"lsfat12: cannot read {image_path}: {exc}")
        return 1
    try:
        bpb = parse_bpb(data)
    except ImageError as exc:
        eprint(f"lsfat12: {exc}")
        return 1

    fat0 = data[bpb["fat_start"] * SECTOR:
                (bpb["fat_start"] + bpb["fat_sectors"]) * SECTOR]

    # --cat / --dump act on one file and skip the listing.
    if action is not None:
        _, name_text, out_path = action
        wanted = parse_wanted(name_text)
        files, _ = parse_root(data, bpb)
        for entry in files:
            if entry["raw"] == wanted and not entry["attr"] & 0x10:
                try:
                    payload = read_file_bytes(data, bpb, fat0, entry)
                except ImageError as exc:
                    eprint(f"lsfat12: {exc}")
                    return 1
                if out_path is None:
                    sys.stdout.buffer.write(payload)
                else:
                    try:
                        with open(out_path, "wb") as handle:
                            handle.write(payload)
                    except OSError as exc:
                        eprint(f"lsfat12: cannot write {out_path}: {exc}")
                        return 1
                    print(f"{show_name(wanted)} -> {out_path} "
                          f"({len(payload)} bytes)")
                return 0
        eprint(f"lsfat12: file not found: {name_text}")
        return 1

    want_chains = "--chains" in flags
    want_check = "--check" in flags

    print(f"{image_path}: {bpb['total']} sectors, "
          f"{bpb['spc']}-sector clusters, {bpb['clusters']} clusters, "
          f"{bpb['roots']} root entries")
    problems = []
    if want_check:
        problems += check_bpb(bpb, len(data))
    if bpb["fats"] == 2:
        fat1 = data[(bpb["fat_start"] + bpb["fat_sectors"]) * SECTOR:
                    (bpb["fat_start"] + 2 * bpb["fat_sectors"]) * SECTOR]
        diffs = [i for i, (a, b) in enumerate(zip(fat0, fat1)) if a != b]
        if diffs:
            print(f"FAT mirrors: MISMATCH at {len(diffs)} bytes "
                  f"(first @0x{diffs[0]:X})")
            problems.append("FAT copies disagree")
        else:
            print("FAT mirrors: match")
    else:
        print("FAT mirrors: single copy")

    files, deleted = parse_root(data, bpb)
    if deleted:
        print(f"deleted entries: {len(deleted)} (clusters held, not checked)")
    print(f"{'NAME':<13}{'ATTR':<6}{'SIZE':>9} {'FIRST':>6} "
          f"{'NCLU':>5}  STATUS")
    lines, file_problems, held = summarize(data, bpb, fat0, files, deleted)
    problems += file_problems

    deleted_held = set()
    for entry in deleted:
        first = struct.unpack_from("<H", entry, 26)[0]
        chain, _ = walk_chain(fat0, first, 10 ** 9, bpb["clusters"])
        deleted_held.update(chain)

    for entry, chain, status in lines:
        name = show_name(entry["raw"])
        nclu = "-" if chain is None else str(len(chain))
        print(f"{name:<13}{show_attr(entry['attr']):<6}"
              f"{entry['size']:>9} {entry['first']:>6} {nclu:>5}  {status}")
        if want_chains and chain:
            print("    chain: " + "->".join(str(c) for c in chain))

    free = bad = used = 0
    orphans = []
    for cluster in range(2, bpb["clusters"] + 2):
        try:
            value = fat12_get(fat0, cluster)
        except ImageError:
            problems.append("FAT table truncated")
            break
        if value == 0:
            free += 1
        elif value == BAD:
            bad += 1
        else:
            used += 1
            if cluster not in held and cluster not in deleted_held:
                orphans.append(cluster)
    print(f"clusters: total={bpb['clusters']} used={used} free={free} "
          f"bad={bad} orphans={len(orphans)}")
    if orphans:
        shown = ",".join(str(c) for c in orphans[:16])
        extra = "..." if len(orphans) > 16 else ""
        print(f"orphaned clusters: {shown}{extra}")
        problems.append(f"{len(orphans)} orphaned clusters")
    if bad:
        problems.append(f"{bad} BAD clusters")

    if want_check:
        if problems:
            print("CHECK: FAILED")
            for problem in problems:
                print(f"  - {problem}")
            return 2
        print("CHECK: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
