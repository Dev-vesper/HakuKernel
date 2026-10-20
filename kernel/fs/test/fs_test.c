#include "fs_test.h"
#include "ata.h"
#include "fat12.h"
#include "fat12_vfs.h"
#include "heap.h"
#include "klog.h"
#include "scheduler.h"
#include "vfs.h"

#define FS_TEST_PID 1u
#define FS_FILL_MAX 600u
#define FS_SECTOR 512u
#define FS_SEEK_SET 0u
#define FS_SEEK_END 2u

#define FS_FAT12_EOC 0xFFFu
#define FS_FAT12_BAD 0xFF7u
#define FS_FAT12_RESERVED 0xFF0u

static int test_done;
static uint8_t scratch[16];
static uint8_t edge_buf[2048];
static uint8_t edge_back[2048];

static int expect_tagged(const char *tag, const char *label, int32_t got,
                         int32_t want) {
    if (got == want) return 1;
    klog_prefix();
    klog_write(tag);
    klog_write(" ");
    klog_write(label);
    klog_write(" got=");
    if (got < 0) {
        klog_putc('-');
        klog_dec((uint32_t)(-got));
    } else {
        klog_dec((uint32_t)got);
    }
    klog_write("\n");
    return 0;
}

static int expect(const char *label, int32_t got, int32_t want) {
    return expect_tagged("FS-ERROR-FAIL", label, got, want);
}

static void fill_name(char name[12], uint32_t i) {
    name[0] = 'F';
    name[1] = (char)('0' + (i / 100u) % 10u);
    name[2] = (char)('0' + (i / 10u) % 10u);
    name[3] = (char)('0' + i % 10u);
    name[4] = '.';
    name[5] = 'T';
    name[6] = 'X';
    name[7] = 'T';
    name[8] = 0;
}

static void fill_pattern(uint8_t *buffer, uint32_t count) {
    for (uint32_t i = 0; i < count; i++)
        buffer[i] = (uint8_t)(i & 0xFFu);
}

static int same_bytes(const uint8_t *a, const uint8_t *b, uint32_t count) {
    for (uint32_t i = 0; i < count; i++)
        if (a[i] != b[i]) return 0;
    return 1;
}

/* Raw 12-bit FAT accessors over a snapshot image (copy 0 layout). */
static uint16_t chain_next(const uint8_t *fat, uint16_t cluster) {
    uint32_t off = (uint32_t)cluster + (uint32_t)cluster / 2u;
    uint16_t raw = (uint16_t)fat[off] | ((uint16_t)fat[off + 1u] << 8);
    return (cluster & 1u) ? (uint16_t)(raw >> 4) : (uint16_t)(raw & 0x0FFFu);
}

static void chain_set(uint8_t *fat, uint16_t cluster, uint16_t value) {
    uint32_t off = (uint32_t)cluster + (uint32_t)cluster / 2u;
    uint16_t raw = (uint16_t)fat[off] | ((uint16_t)fat[off + 1u] << 8);
    if (cluster & 1u)
        raw = (uint16_t)((raw & 0x000Fu) | (value << 4));
    else
        raw = (uint16_t)((raw & 0xF000u) | (value & 0x0FFFu));
    fat[off] = (uint8_t)raw;
    fat[off + 1u] = (uint8_t)(raw >> 8);
}

typedef struct {
    uint8_t *image;
    uint32_t sectors;
    uint32_t fat_start;
    uint32_t fat_count;
    uint32_t cluster_count;
} fat_snap_t;

static int snap_take(fat_snap_t *snap) {
    uint32_t sectors_per_cluster;
    memset(snap, 0, sizeof(*snap));
    if (fat12_test_sector_info(&snap->fat_start, &snap->sectors,
                               &snap->fat_count, &snap->cluster_count,
                               &sectors_per_cluster) != 1)
        return 0;
    snap->image = (uint8_t *)kmalloc(snap->sectors * FS_SECTOR);
    if (!snap->image) return 0;
    for (uint32_t i = 0; i < snap->sectors; i++) {
        if (fat12_test_read_sector(snap->fat_start + i,
                                   snap->image + i * FS_SECTOR) != 1) {
            kfree(snap->image);
            snap->image = 0;
            return 0;
        }
    }
    return 1;
}

static void snap_free(fat_snap_t *snap) {
    if (snap->image) kfree(snap->image);
    memset(snap, 0, sizeof(*snap));
}

/* Mirror one image to every FAT copy so the volume stays consistent
 * while patched, and pristine again after restore. */
static int snap_write_all(const fat_snap_t *snap, const uint8_t *image) {
    for (uint32_t copy = 0; copy < snap->fat_count; copy++) {
        for (uint32_t i = 0; i < snap->sectors; i++) {
            if (fat12_test_write_sector(snap->fat_start +
                                        copy * snap->sectors + i,
                                        image + i * FS_SECTOR) != 1)
                return 0;
        }
    }
    return 1;
}

static int test_empty_file(void) {
    int ok = 1;
    int32_t fd;
    int32_t r;
    fd = vfs_open(FS_TEST_PID, "EMPTY.TXT", VFS_O_CREAT | VFS_O_RDWR);
    ok &= expect_tagged("FS-EDGE-FAIL", "empty-create", fd >= 3 ? 0 : fd, 0);
    if (fd < 3) return 0;
    /* A fresh file is already at EOF: reads return 0, END is 0. */
    ok &= expect_tagged("FS-EDGE-FAIL", "empty-read",
                        vfs_read(FS_TEST_PID, (uint32_t)fd, scratch,
                                 sizeof(scratch)),
                        0);
    ok &= expect_tagged("FS-EDGE-FAIL", "empty-end",
                        vfs_seek(FS_TEST_PID, (uint32_t)fd, 0, FS_SEEK_END),
                        0);
    scratch[0] = 'Q';
    ok &= expect_tagged("FS-EDGE-FAIL", "empty-write",
                        vfs_write(FS_TEST_PID, (uint32_t)fd, scratch, 1), 1);
    ok &= expect_tagged("FS-EDGE-FAIL", "empty-rewind",
                        vfs_seek(FS_TEST_PID, (uint32_t)fd, 0, FS_SEEK_SET),
                        0);
    scratch[0] = 0;
    r = vfs_read(FS_TEST_PID, (uint32_t)fd, scratch, 1);
    ok &= expect_tagged("FS-EDGE-FAIL", "empty-reread", r, 1);
    ok &= expect_tagged("FS-EDGE-FAIL", "empty-byte", scratch[0], 'Q');
    ok &= expect_tagged("FS-EDGE-FAIL", "empty-size",
                        vfs_seek(FS_TEST_PID, (uint32_t)fd, 0, FS_SEEK_END),
                        1);
    ok &= expect_tagged("FS-EDGE-FAIL", "empty-close",
                        vfs_close(FS_TEST_PID, (uint32_t)fd), 0);
    return ok;
}

static int test_sector_edges(void) {
    int ok = 1;
    int32_t fd;
    int32_t r;
    fill_pattern(edge_buf, sizeof(edge_buf));
    fd = vfs_open(FS_TEST_PID, "EDGE.TXT", VFS_O_CREAT | VFS_O_RDWR);
    ok &= expect_tagged("FS-EDGE-FAIL", "edge-create", fd >= 3 ? 0 : fd, 0);
    if (fd < 3) return 0;
    /* Exact-sector write, then a two-sector write. */
    ok &= expect_tagged("FS-EDGE-FAIL", "edge-write2sectors",
                        vfs_write(FS_TEST_PID, (uint32_t)fd, edge_buf,
                                  2u * FS_SECTOR),
                        (int32_t)(2u * FS_SECTOR));
    ok &= expect_tagged("FS-EDGE-FAIL", "edge-rewind",
                        vfs_seek(FS_TEST_PID, (uint32_t)fd, 0, FS_SEEK_SET),
                        0);
    r = vfs_read(FS_TEST_PID, (uint32_t)fd, edge_back, 2u * FS_SECTOR);
    ok &= expect_tagged("FS-EDGE-FAIL", "edge-read2sectors", r,
                        (int32_t)(2u * FS_SECTOR));
    ok &= expect_tagged("FS-EDGE-FAIL", "edge-compare",
                        same_bytes(edge_back, edge_buf, 2u * FS_SECTOR), 1);
    /* A read straddling the 512-byte boundary must splice both sectors. */
    ok &= expect_tagged("FS-EDGE-FAIL", "edge-seek511",
                        vfs_seek(FS_TEST_PID, (uint32_t)fd, 511, FS_SEEK_SET),
                        511);
    r = vfs_read(FS_TEST_PID, (uint32_t)fd, edge_back, 2);
    ok &= expect_tagged("FS-EDGE-FAIL", "edge-spanread", r, 2);
    ok &= expect_tagged("FS-EDGE-FAIL", "edge-spanlo", edge_back[0], 0xFF);
    ok &= expect_tagged("FS-EDGE-FAIL", "edge-spanhi", edge_back[1], 0x00);
    /* Unaligned partial-sector overwrite (read-modify-write). */
    ok &= expect_tagged("FS-EDGE-FAIL", "edge-seek100",
                        vfs_seek(FS_TEST_PID, (uint32_t)fd, 100, FS_SEEK_SET),
                        100);
    memset(edge_back, 0xAA, 10);
    ok &= expect_tagged("FS-EDGE-FAIL", "edge-partial-write",
                        vfs_write(FS_TEST_PID, (uint32_t)fd, edge_back, 10),
                        10);
    ok &= expect_tagged("FS-EDGE-FAIL", "edge-size",
                        vfs_seek(FS_TEST_PID, (uint32_t)fd, 0, FS_SEEK_END),
                        (int32_t)(2u * FS_SECTOR));
    ok &= expect_tagged("FS-EDGE-FAIL", "edge-close",
                        vfs_close(FS_TEST_PID, (uint32_t)fd), 0);
    return ok;
}

/* Build a 3-cluster scratch file through the public VFS interface, then
 * locate its chain with the read-only debug hook for fault injection.
 * Returns 1 or a negative -VFS_E* errno. */
static int open_scratch_chain(const char *name, uint16_t *first_out) {
    int32_t fd;
    int32_t written;
    int looked_up;
    fill_pattern(edge_buf, 3u * FS_SECTOR);
    fd = vfs_open(FS_TEST_PID, name,
                  VFS_O_CREAT | VFS_O_RDWR | VFS_O_TRUNC);
    if (fd < 3) return fd;
    written = vfs_write(FS_TEST_PID, (uint32_t)fd, edge_buf,
                        3u * FS_SECTOR);
    vfs_close(FS_TEST_PID, (uint32_t)fd);
    if (written != (int32_t)(3u * FS_SECTOR)) return -VFS_EIO;
    looked_up = fat12_debug_first_cluster(name, first_out);
    if (looked_up != 1) return fat12_map_errno(looked_up);
    return 1;
}

static int heal_check(const char *label, uint32_t position, uint32_t count) {
    int32_t fd;
    int32_t r;
    int ok = 1;
    fd = vfs_open(FS_TEST_PID, "CHAIN.TXT", VFS_O_RDONLY);
    if (fd < 3) {
        expect_tagged("FS-CORRUPT-FAIL", label, fd, 3);
        return 0;
    }
    if (vfs_seek(FS_TEST_PID, (uint32_t)fd, (int32_t)position,
                 FS_SEEK_SET) != (int32_t)position) {
        expect_tagged("FS-CORRUPT-FAIL", label, -999, (int32_t)position);
        vfs_close(FS_TEST_PID, (uint32_t)fd);
        return 0;
    }
    r = vfs_read(FS_TEST_PID, (uint32_t)fd, edge_back, count);
    ok &= expect_tagged("FS-CORRUPT-FAIL", label, r, (int32_t)count);
    if (r > 0)
        ok &= expect_tagged("FS-CORRUPT-FAIL", label,
                            same_bytes(edge_back, edge_buf + position,
                                       (uint32_t)r),
                            1);
    vfs_close(FS_TEST_PID, (uint32_t)fd);
    return ok;
}

static int test_corrupt_chains(void) {
    int ok = 1;
    fat_snap_t snap;
    uint8_t *work = 0;
    uint16_t first = 0;
    uint32_t cl_b = 0;
    uint32_t cl_c = 0;
    uint32_t cl_d = 0;
    uint16_t b = 0;
    uint16_t c = 0;
    int32_t fd;
    int setup_result;
    /* FULLD.TXT is only a directory entry until the data-full probe, so
     * creating it up front keeps the later FAT snapshot accurate. */
    fd = vfs_open(FS_TEST_PID, "FULLD.TXT", VFS_O_CREAT | VFS_O_RDWR);
    ok &= expect_tagged("FS-CORRUPT-FAIL", "corrupt-fulld-create",
                        fd >= 3 ? 0 : fd, 0);
    if (fd >= 3) vfs_close(FS_TEST_PID, (uint32_t)fd);
    else return 0;
    setup_result = open_scratch_chain("CHAIN.TXT", &first);
    ok &= expect_tagged("FS-CORRUPT-FAIL", "corrupt-setup", setup_result, 1);
    if (setup_result != 1) return 0;
    if (!snap_take(&snap)) {
        expect_tagged("FS-CORRUPT-FAIL", "corrupt-snap", 0, 1);
        return 0;
    }
    work = (uint8_t *)kmalloc(snap.sectors * FS_SECTOR);
    if (!work) {
        expect_tagged("FS-CORRUPT-FAIL", "corrupt-work", 0, 1);
        snap_free(&snap);
        return 0;
    }
    /* The scratch file must own exactly three chained clusters. Each
     * hop is range-checked before it is followed, so a surprising
     * on-disk value can never walk the reader out of the snapshot. */
    ok &= expect_tagged("FS-CORRUPT-FAIL", "corrupt-chain-a",
                        first >= 2u && first < snap.cluster_count + 2u, 1);
    if (ok) {
        cl_b = chain_next(snap.image, first);
        ok &= expect_tagged("FS-CORRUPT-FAIL", "corrupt-chain-b",
                            cl_b >= 2u && cl_b < snap.cluster_count + 2u, 1);
    }
    if (ok) {
        cl_c = chain_next(snap.image, (uint16_t)cl_b);
        ok &= expect_tagged("FS-CORRUPT-FAIL", "corrupt-chain-c",
                            cl_c >= 2u && cl_c < snap.cluster_count + 2u, 1);
    }
    if (ok) {
        cl_d = chain_next(snap.image, (uint16_t)cl_c);
        ok &= expect_tagged("FS-CORRUPT-FAIL", "corrupt-chain-len",
                            cl_d == FS_FAT12_EOC, 1);
    }
    if (!ok) {
        kfree(work);
        snap_free(&snap);
        return 0;
    }
    b = (uint16_t)cl_b;
    c = (uint16_t)cl_c;

    /* T1: chain cut short (EOC where the next cluster belongs). Reads
     * past the cut must fail with EIO instead of wandering the disk. */
    memcpy(work, snap.image, snap.sectors * FS_SECTOR);
    chain_set(work, first, FS_FAT12_EOC);
    ok &= expect_tagged("FS-CORRUPT-FAIL", "trunc-patch",
                        snap_write_all(&snap, work), 1);
    fd = vfs_open(FS_TEST_PID, "CHAIN.TXT", VFS_O_RDONLY);
    ok &= expect_tagged("FS-CORRUPT-FAIL", "trunc-open", fd >= 3 ? 0 : fd,
                        0);
    if (fd >= 3) {
        ok &= expect_tagged("FS-CORRUPT-FAIL", "trunc-seek",
                            vfs_seek(FS_TEST_PID, (uint32_t)fd, 600,
                                     FS_SEEK_SET),
                            600);
        ok &= expect_tagged("FS-CORRUPT-FAIL", "trunc-read-eio",
                            vfs_read(FS_TEST_PID, (uint32_t)fd, edge_back,
                                     100),
                            -VFS_EIO);
        vfs_close(FS_TEST_PID, (uint32_t)fd);
    } else {
        ok = 0;
    }
    ok &= expect_tagged("FS-CORRUPT-FAIL", "trunc-restore",
                        snap_write_all(&snap, snap.image), 1);
    ok &= heal_check("trunc-heal", 600, 100);

    /* T2: FAT12 bad-cluster marker mid-chain. */
    memcpy(work, snap.image, snap.sectors * FS_SECTOR);
    chain_set(work, b, FS_FAT12_BAD);
    ok &= expect_tagged("FS-CORRUPT-FAIL", "bad-patch",
                        snap_write_all(&snap, work), 1);
    fd = vfs_open(FS_TEST_PID, "CHAIN.TXT", VFS_O_RDONLY);
    ok &= expect_tagged("FS-CORRUPT-FAIL", "bad-open", fd >= 3 ? 0 : fd, 0);
    if (fd >= 3) {
        ok &= expect_tagged("FS-CORRUPT-FAIL", "bad-seek",
                            vfs_seek(FS_TEST_PID, (uint32_t)fd, 1200,
                                     FS_SEEK_SET),
                            1200);
        ok &= expect_tagged("FS-CORRUPT-FAIL", "bad-read-eio",
                            vfs_read(FS_TEST_PID, (uint32_t)fd, edge_back,
                                     100),
                            -VFS_EIO);
        vfs_close(FS_TEST_PID, (uint32_t)fd);
    } else {
        ok = 0;
    }
    ok &= expect_tagged("FS-CORRUPT-FAIL", "bad-restore",
                        snap_write_all(&snap, snap.image), 1);
    ok &= heal_check("bad-heal", 1200, 100);

    /* T3: reserved cluster value mid-chain (in range encoding, but no
     * such cluster exists on this volume). */
    memcpy(work, snap.image, snap.sectors * FS_SECTOR);
    chain_set(work, b, FS_FAT12_RESERVED);
    ok &= expect_tagged("FS-CORRUPT-FAIL", "reserved-patch",
                        snap_write_all(&snap, work), 1);
    fd = vfs_open(FS_TEST_PID, "CHAIN.TXT", VFS_O_RDONLY);
    ok &= expect_tagged("FS-CORRUPT-FAIL", "reserved-open",
                        fd >= 3 ? 0 : fd, 0);
    if (fd >= 3) {
        ok &= expect_tagged("FS-CORRUPT-FAIL", "reserved-seek",
                            vfs_seek(FS_TEST_PID, (uint32_t)fd, 1200,
                                     FS_SEEK_SET),
                            1200);
        ok &= expect_tagged("FS-CORRUPT-FAIL", "reserved-read-eio",
                            vfs_read(FS_TEST_PID, (uint32_t)fd, edge_back,
                                     100),
                            -VFS_EIO);
        vfs_close(FS_TEST_PID, (uint32_t)fd);
    } else {
        ok = 0;
    }
    ok &= expect_tagged("FS-CORRUPT-FAIL", "reserved-restore",
                        snap_write_all(&snap, snap.image), 1);
    ok &= heal_check("reserved-heal", 1200, 100);

    /* T4: back edge (last cluster points at the middle one). Bounded
     * walks must terminate: reads succeed on the valid prefix, and
     * freeing the loop must fail cleanly instead of spinning. The
     * failed TRUNC frees what it walked and zeroes the entry, so the
     * file comes back as a clean empty file that plain writes heal —
     * no restore needed, nothing leaks. */
    memcpy(work, snap.image, snap.sectors * FS_SECTOR);
    chain_set(work, c, b);
    ok &= expect_tagged("FS-CORRUPT-FAIL", "cycle-patch",
                        snap_write_all(&snap, work), 1);
    ok &= heal_check("cycle-read-terminates", 0, 3u * FS_SECTOR);
    ok &= expect_tagged("FS-CORRUPT-FAIL", "cycle-trunc-eio",
                        vfs_open(FS_TEST_PID, "CHAIN.TXT",
                                 VFS_O_RDWR | VFS_O_TRUNC),
                        -VFS_EIO);
    fd = vfs_open(FS_TEST_PID, "CHAIN.TXT", VFS_O_RDONLY);
    ok &= expect_tagged("FS-CORRUPT-FAIL", "cycle-empty-open",
                        fd >= 3 ? 0 : fd, 0);
    if (fd >= 3) {
        ok &= expect_tagged("FS-CORRUPT-FAIL", "cycle-empty-size",
                            vfs_seek(FS_TEST_PID, (uint32_t)fd, 0,
                                     FS_SEEK_END),
                            0);
        vfs_close(FS_TEST_PID, (uint32_t)fd);
    } else {
        ok = 0;
    }
    /* Heal with ordinary writes: the freed loop clusters are reusable
     * and the entry is already a valid empty file. */
    fd = vfs_open(FS_TEST_PID, "CHAIN.TXT",
                  VFS_O_CREAT | VFS_O_RDWR | VFS_O_TRUNC);
    ok &= expect_tagged("FS-CORRUPT-FAIL", "cycle-recreate",
                        fd >= 3 ? 0 : fd, 0);
    if (fd >= 3) {
        fill_pattern(edge_buf, 3u * FS_SECTOR);
        ok &= expect_tagged("FS-CORRUPT-FAIL", "cycle-rewrite",
                            vfs_write(FS_TEST_PID, (uint32_t)fd, edge_buf,
                                      3u * FS_SECTOR),
                            (int32_t)(3u * FS_SECTOR));
        vfs_close(FS_TEST_PID, (uint32_t)fd);
    } else {
        ok = 0;
    }
    ok &= heal_check("cycle-heal", 0, 3u * FS_SECTOR);

    /* T5: every data cluster marked used. Extending any file must
     * report a full disk instead of an I/O error. Re-snapshot first:
     * the T4 rewrite above legitimately reallocated the scratch chain,
     * so the original image no longer describes this volume. */
    snap_free(&snap);
    if (ok && !snap_take(&snap)) {
        expect_tagged("FS-CORRUPT-FAIL", "datafull-snap", 0, 1);
        ok = 0;
    }
    if (!ok) {
        kfree(work);
        snap_free(&snap);
        return 0;
    }
    memcpy(work, snap.image, snap.sectors * FS_SECTOR);
    for (uint16_t cluster = 2; cluster < snap.cluster_count + 2u;
         cluster++)
        chain_set(work, cluster, FS_FAT12_EOC);
    ok &= expect_tagged("FS-CORRUPT-FAIL", "datafull-patch",
                        snap_write_all(&snap, work), 1);
    fd = vfs_open(FS_TEST_PID, "FULLD.TXT", VFS_O_RDWR);
    ok &= expect_tagged("FS-CORRUPT-FAIL", "datafull-open",
                        fd >= 3 ? 0 : fd, 0);
    if (fd >= 3) {
        scratch[0] = 'Z';
        ok &= expect_tagged("FS-CORRUPT-FAIL", "datafull-write-enospc",
                            vfs_write(FS_TEST_PID, (uint32_t)fd, scratch, 1),
                            -VFS_ENOSPC);
        vfs_close(FS_TEST_PID, (uint32_t)fd);
    } else {
        ok = 0;
    }
    ok &= expect_tagged("FS-CORRUPT-FAIL", "datafull-restore",
                        snap_write_all(&snap, snap.image), 1);
    fd = vfs_open(FS_TEST_PID, "FULLD.TXT", VFS_O_RDWR);
    ok &= expect_tagged("FS-CORRUPT-FAIL", "datafull-heal-open",
                        fd >= 3 ? 0 : fd, 0);
    if (fd >= 3) {
        scratch[0] = 'Z';
        ok &= expect_tagged("FS-CORRUPT-FAIL", "datafull-heal-write",
                            vfs_write(FS_TEST_PID, (uint32_t)fd, scratch, 1),
                            1);
        vfs_close(FS_TEST_PID, (uint32_t)fd);
    } else {
        ok = 0;
    }

    kfree(work);
    snap_free(&snap);
    return ok;
}

static int craft_disk_full(fat_snap_t *snap, uint8_t *work) {
    memcpy(work, snap->image, snap->sectors * FS_SECTOR);
    for (uint16_t cluster = 2; cluster < snap->cluster_count + 2u;
         cluster++)
        chain_set(work, cluster, FS_FAT12_EOC);
    return snap_write_all(snap, work);
}

/* Failed-write cleanup: a gap that cannot be zeroed must not grow the
 * size, a partially written payload keeps exactly its durable prefix,
 * and a TRUNC that cannot free its chain must leave consistent-as-empty
 * metadata instead of a broken chain. */
static int test_write_rollback(void) {
    int ok = 1;
    fat_snap_t snap;
    uint8_t *work = 0;
    uint16_t first = 0;
    uint32_t cl_b = 0;
    uint32_t cl_c = 0;
    int32_t fd;
    int32_t r;
    int setup_result;
    if (!snap_take(&snap)) {
        expect_tagged("FS-ROLLBACK-FAIL", "rollback-snap", 0, 1);
        return 0;
    }
    work = (uint8_t *)kmalloc(snap.sectors * FS_SECTOR);
    if (!work) {
        expect_tagged("FS-ROLLBACK-FAIL", "rollback-work", 0, 1);
        snap_free(&snap);
        return 0;
    }

    /* Scratch files up front so the snapshots below cover their chains.
     * ROLL stays empty; TRUNC owns three clusters for the cycle probe. */
    fd = vfs_open(FS_TEST_PID, "ROLL.TXT", VFS_O_CREAT | VFS_O_RDWR);
    ok &= expect_tagged("FS-ROLLBACK-FAIL", "rollback-roll-create",
                        fd >= 3 ? 0 : fd, 0);
    if (fd >= 3) vfs_close(FS_TEST_PID, (uint32_t)fd);
    else {
        ok = 0;
        goto done;
    }
    setup_result = open_scratch_chain("TRUNC.TXT", &first);
    ok &= expect_tagged("FS-ROLLBACK-FAIL", "rollback-trunc-setup",
                        setup_result, 1);
    if (setup_result != 1) {
        ok = 0;
        goto done;
    }
    snap_free(&snap);
    if (!snap_take(&snap)) {
        expect_tagged("FS-ROLLBACK-FAIL", "rollback-snap2", 0, 1);
        ok = 0;
        goto done;
    }

    /* R1: seek past EOF on a full volume, then write. The gap cannot
     * be zeroed, so the size must stay 0 and the error stays ENOSPC. */
    ok &= expect_tagged("FS-ROLLBACK-FAIL", "gap-patch",
                        craft_disk_full(&snap, work), 1);
    fd = vfs_open(FS_TEST_PID, "ROLL.TXT", VFS_O_RDWR);
    ok &= expect_tagged("FS-ROLLBACK-FAIL", "gap-open",
                        fd >= 3 ? 0 : fd, 0);
    if (fd >= 3) {
        ok &= expect_tagged("FS-ROLLBACK-FAIL", "gap-seek",
                            vfs_seek(FS_TEST_PID, (uint32_t)fd, 2048,
                                     FS_SEEK_SET),
                            2048);
        ok &= expect_tagged("FS-ROLLBACK-FAIL", "gap-write-enospc",
                            vfs_write(FS_TEST_PID, (uint32_t)fd, edge_buf,
                                      10),
                            -VFS_ENOSPC);
        ok &= expect_tagged("FS-ROLLBACK-FAIL", "gap-size-kept",
                            vfs_seek(FS_TEST_PID, (uint32_t)fd, 0,
                                     FS_SEEK_END),
                            0);
        vfs_close(FS_TEST_PID, (uint32_t)fd);
    } else {
        ok = 0;
    }
    ok &= expect_tagged("FS-ROLLBACK-FAIL", "gap-restore",
                        snap_write_all(&snap, snap.image), 1);
    /* The volume heals: a plain small write works again. */
    fd = vfs_open(FS_TEST_PID, "ROLL.TXT", VFS_O_RDWR);
    ok &= expect_tagged("FS-ROLLBACK-FAIL", "gap-heal-open",
                        fd >= 3 ? 0 : fd, 0);
    if (fd >= 3) {
        fill_pattern(edge_buf, 10);
        ok &= expect_tagged("FS-ROLLBACK-FAIL", "gap-heal-write",
                            vfs_write(FS_TEST_PID, (uint32_t)fd, edge_buf,
                                      10),
                            10);
        vfs_close(FS_TEST_PID, (uint32_t)fd);
    } else {
        ok = 0;
    }

    /* R2: the first cluster exists, the second cannot be allocated.
     * Exactly the durable 512-byte prefix must survive with ENOSPC
     * reported via a short count, and the size must cover it. */
    snap_free(&snap);
    if (!snap_take(&snap)) {
        expect_tagged("FS-ROLLBACK-FAIL", "rollback-snap3", 0, 1);
        ok = 0;
        goto done;
    }
    ok &= expect_tagged("FS-ROLLBACK-FAIL", "partial-patch",
                        craft_disk_full(&snap, work), 1);
    fill_pattern(edge_buf, 600);
    fd = vfs_open(FS_TEST_PID, "ROLL.TXT", VFS_O_RDWR);
    ok &= expect_tagged("FS-ROLLBACK-FAIL", "partial-open",
                        fd >= 3 ? 0 : fd, 0);
    if (fd >= 3) {
        ok &= expect_tagged("FS-ROLLBACK-FAIL", "partial-seek",
                            vfs_seek(FS_TEST_PID, (uint32_t)fd, 0,
                                     FS_SEEK_SET),
                            0);
        ok &= expect_tagged("FS-ROLLBACK-FAIL", "partial-short",
                            vfs_write(FS_TEST_PID, (uint32_t)fd, edge_buf,
                                      600),
                            512);
        ok &= expect_tagged("FS-ROLLBACK-FAIL", "partial-size",
                            vfs_seek(FS_TEST_PID, (uint32_t)fd, 0,
                                     FS_SEEK_END),
                            512);
        ok &= expect_tagged("FS-ROLLBACK-FAIL", "partial-rewind",
                            vfs_seek(FS_TEST_PID, (uint32_t)fd, 0,
                                     FS_SEEK_SET),
                            0);
        r = vfs_read(FS_TEST_PID, (uint32_t)fd, edge_back, 512);
        ok &= expect_tagged("FS-ROLLBACK-FAIL", "partial-reread", r, 512);
        if (r > 0)
            ok &= expect_tagged("FS-ROLLBACK-FAIL", "partial-content",
                                same_bytes(edge_back, edge_buf, 512), 1);
        vfs_close(FS_TEST_PID, (uint32_t)fd);
    } else {
        ok = 0;
    }
    ok &= expect_tagged("FS-ROLLBACK-FAIL", "partial-restore",
                        snap_write_all(&snap, snap.image), 1);

    /* R3: TRUNC over a cyclic chain cannot free it; the file must read
     * back as a clean empty file, not phantom data or EIO. */
    if (ok) {
        if (first >= 2u && first < snap.cluster_count + 2u)
            cl_b = chain_next(snap.image, first);
        if (cl_b >= 2u && cl_b < snap.cluster_count + 2u)
            cl_c = chain_next(snap.image, (uint16_t)cl_b);
        ok &= expect_tagged("FS-ROLLBACK-FAIL", "trunc-chain",
                            cl_c >= 2u &&
                            cl_c < snap.cluster_count + 2u,
                            1);
    }
    if (!ok) goto done;
    memcpy(work, snap.image, snap.sectors * FS_SECTOR);
    chain_set(work, (uint16_t)cl_c, (uint16_t)cl_b);
    ok &= expect_tagged("FS-ROLLBACK-FAIL", "trunc-cycle-patch",
                        snap_write_all(&snap, work), 1);
    ok &= expect_tagged("FS-ROLLBACK-FAIL", "trunc-open-eio",
                        vfs_open(FS_TEST_PID, "TRUNC.TXT",
                                 VFS_O_RDWR | VFS_O_TRUNC),
                        -VFS_EIO);
    /* No restore: the failed free already released the loop and the
     * directory entry is consistent-as-empty by construction. */
    fd = vfs_open(FS_TEST_PID, "TRUNC.TXT", VFS_O_RDONLY);
    ok &= expect_tagged("FS-ROLLBACK-FAIL", "trunc-reopen",
                        fd >= 3 ? 0 : fd, 0);
    if (fd >= 3) {
        ok &= expect_tagged("FS-ROLLBACK-FAIL", "trunc-empty",
                            vfs_seek(FS_TEST_PID, (uint32_t)fd, 0,
                                     FS_SEEK_END),
                            0);
        ok &= expect_tagged("FS-ROLLBACK-FAIL", "trunc-eof",
                            vfs_read(FS_TEST_PID, (uint32_t)fd, edge_back,
                                     10),
                            0);
        vfs_close(FS_TEST_PID, (uint32_t)fd);
    } else {
        ok = 0;
    }

done:
    if (work) kfree(work);
    snap_free(&snap);
    return ok;
}

static int test_stat_metadata(void) {
    int ok = 1;
    int32_t fd;
    vfs_stat_t st;

    fd = vfs_open(FS_TEST_PID, "STAT.TXT", VFS_O_CREAT | VFS_O_RDWR);
    ok &= expect_tagged("FS-STAT-FAIL", "stat-create", fd >= 3 ? 0 : fd, 0);
    if (fd < 3) return 0;
    ok &= expect_tagged("FS-STAT-FAIL", "stat-write",
                        vfs_write(FS_TEST_PID, (uint32_t)fd, edge_buf, 700u),
                        700);
    ok &= expect_tagged("FS-STAT-FAIL", "stat-fstat",
                        vfs_fstat(FS_TEST_PID, (uint32_t)fd, &st), 1);
    ok &= expect_tagged("FS-STAT-FAIL", "stat-fstat-mode",
                        (st.st_mode & VFS_S_IFREG) ? 0 : 1, 0);
    ok &= expect_tagged("FS-STAT-FAIL", "stat-fstat-size",
                        st.st_size == 700u ? 0 : 1, 0);
    ok &= expect_tagged("FS-STAT-FAIL", "stat-fstat-nlink",
                        st.st_nlink == 1u ? 0 : 1, 0);
    ok &= expect_tagged("FS-STAT-FAIL", "stat-fstat-blksize",
                        st.st_blksize == 512u ? 0 : 1, 0);
    ok &= expect_tagged("FS-STAT-FAIL", "stat-fstat-blocks",
                        st.st_blocks == 2u ? 0 : 1, 0);
    ok &= expect_tagged("FS-STAT-FAIL", "stat-fstat-owner",
                        (st.st_uid == 0u && st.st_gid == 0u) ? 0 : 1, 0);
    ok &= expect_tagged("FS-STAT-FAIL", "stat-fstat-close",
                        vfs_close(FS_TEST_PID, (uint32_t)fd), 0);

    ok &= expect_tagged("FS-STAT-FAIL", "stat-path",
                        vfs_stat(FS_TEST_PID, "STAT.TXT", &st), 1);
    ok &= expect_tagged("FS-STAT-FAIL", "stat-path-mode",
                        (st.st_mode & VFS_S_IFREG) ? 0 : 1, 0);
    ok &= expect_tagged("FS-STAT-FAIL", "stat-path-size",
                        st.st_size == 700u ? 0 : 1, 0);
    ok &= expect_tagged("FS-STAT-FAIL", "stat-path-ino",
                        st.st_ino != 0u ? 0 : 1, 0);

    ok &= expect_tagged("FS-STAT-FAIL", "stat-dir",
                        vfs_stat(FS_TEST_PID, "/", &st), 1);
    ok &= expect_tagged("FS-STAT-FAIL", "stat-dir-mode",
                        (st.st_mode & VFS_S_IFDIR) ? 0 : 1, 0);
    ok &= expect_tagged("FS-STAT-FAIL", "stat-dir-nlink",
                        st.st_nlink == 1u ? 0 : 1, 0);

    ok &= expect_tagged("FS-STAT-FAIL", "stat-missing",
                        vfs_stat(FS_TEST_PID, "NOSUCH.TXT", &st), -VFS_ENOENT);
    ok &= expect_tagged("FS-STAT-FAIL", "stat-unlink",
                        vfs_unlink(FS_TEST_PID, "STAT.TXT"), 1);
    return ok;
}

static int test_ata_errors(void) {
    int ok = 1;
    uint32_t total = ata_sector_count();
    ok &= expect_tagged("FS-ATA-FAIL", "ata-present", total != 0, 1);
    ok &= expect_tagged("FS-ATA-FAIL", "ata-device-count",
                        ata_device_count() != 0, 1);
    /* Out-of-range, overflowing, empty, and NULL transfers are rejected
     * before touching the device; genuine device faults surface as 0
     * and become EIO upstairs. Sector 0 must stay readable. */
    ok &= expect_tagged("FS-ATA-FAIL", "ata-read-past-end",
                        ata_read_sectors(total, 1, edge_back) ? 1 : 0, 0);
    ok &= expect_tagged("FS-ATA-FAIL", "ata-read-overflow",
                        ata_read_sectors(0xFFFFFF00u, 200, edge_back) ? 1 : 0,
                        0);
    ok &= expect_tagged("FS-ATA-FAIL", "ata-read-zero-count",
                        ata_read_sectors(0, 0, edge_back) ? 1 : 0, 0);
    ok &= expect_tagged("FS-ATA-FAIL", "ata-read-null",
                        ata_read_sectors(0, 1, 0) ? 1 : 0, 0);
    ok &= expect_tagged("FS-ATA-FAIL", "ata-write-past-end",
                        ata_write_sectors(total, 1, edge_back) ? 1 : 0, 0);
    ok &= expect_tagged("FS-ATA-FAIL", "ata-write-null",
                        ata_write_sectors(0, 1, 0) ? 1 : 0, 0);
    ok &= expect_tagged("FS-ATA-FAIL", "fat-sector-past-end",
                        fat12_test_read_sector(total, edge_back),
                        -FAT12_ERR_INVALID);
    ok &= expect_tagged("FS-ATA-FAIL", "fat-sector-null",
                        fat12_test_read_sector(0, 0), -FAT12_ERR_INVALID);
    ok &= expect_tagged("FS-ATA-FAIL", "fat-sector-zero-ok",
                        fat12_test_read_sector(0, edge_back), 1);
    return ok;
}

/* Read-only mount: reads and metadata stay available while every mutating
 * path reports EROFS. A writable descriptor opened before the remount must
 * fail its writes too, and remounting read-write restores the write path. */
static int test_read_only(void) {
    int pass = 1;
    int32_t fd;
    int32_t writable;

    writable = vfs_open(FS_TEST_PID, "HAKU.ELF", VFS_O_RDWR);
    pass &= expect("ro-preopen", writable >= 3 ? 0 : writable, 0);

    pass &= expect("ro-mount",
                   vfs_mount_root_flags("fat12", VFS_MOUNT_RDONLY), 1);

    fd = vfs_open(FS_TEST_PID, "HAKU.ELF", VFS_O_RDONLY);
    pass &= expect("ro-open-read", fd >= 3 ? 0 : fd, 0);
    if (fd >= 3) {
        pass &= expect("ro-read",
                       vfs_read(FS_TEST_PID, (uint32_t)fd, scratch, 4), 4);
        pass &= expect("ro-close",
                       vfs_close(FS_TEST_PID, (uint32_t)fd), 0);
    } else {
        pass = 0;
    }

    pass &= expect("ro-open-write",
                   vfs_open(FS_TEST_PID, "HAKU.ELF", VFS_O_WRONLY),
                   -VFS_EROFS);
    pass &= expect("ro-create",
                   vfs_open(FS_TEST_PID, "RO.TXT",
                            VFS_O_CREAT | VFS_O_WRONLY),
                   -VFS_EROFS);
    pass &= expect("ro-trunc",
                   vfs_open(FS_TEST_PID, "HAKU.ELF",
                            VFS_O_RDWR | VFS_O_TRUNC),
                   -VFS_EROFS);
    pass &= expect("ro-unlink", vfs_unlink(FS_TEST_PID, "HAKU.ELF"),
                   -VFS_EROFS);
    pass &= expect("ro-rename",
                   vfs_rename(FS_TEST_PID, "HAKU.ELF", "RO.ELF"),
                   -VFS_EROFS);
    pass &= expect("ro-mkdir", vfs_mkdir(FS_TEST_PID, "RODIR"), -VFS_EROFS);
    pass &= expect("ro-rmdir", vfs_rmdir(FS_TEST_PID, "RODIR"), -VFS_EROFS);

    if (writable >= 3) {
        pass &= expect("ro-late-write",
                       vfs_write(FS_TEST_PID, (uint32_t)writable, scratch, 1),
                       -VFS_EROFS);
        pass &= expect("ro-late-close",
                       vfs_close(FS_TEST_PID, (uint32_t)writable), 0);
    } else {
        pass = 0;
    }

    pass &= expect("ro-remount", vfs_mount_root_flags("fat12", 0), 1);
    fd = vfs_open(FS_TEST_PID, "RO.TXT", VFS_O_CREAT | VFS_O_RDWR);
    pass &= expect("rw-create", fd >= 3 ? 0 : fd, 0);
    if (fd >= 3) {
        pass &= expect("rw-write",
                       vfs_write(FS_TEST_PID, (uint32_t)fd, scratch, 1), 1);
        pass &= expect("rw-close", vfs_close(FS_TEST_PID, (uint32_t)fd), 0);
        pass &= expect("rw-unlink", vfs_unlink(FS_TEST_PID, "RO.TXT"), 1);
    } else {
        pass = 0;
    }
    return pass;
}

void fs_error_self_test(void) {
    int32_t fd;
    int pass = 1;
    if (test_done || !scheduler_user_tasks_done()) return;
    test_done = 1;

    /* Automatic close-on-exit: by the time only the idle task runs, every
     * process that ran (the initial program and the spawned child) must
     * have had all of its descriptors -- standard 0/1/2 and any files --
     * released, so their tables are empty. */
    pass &= expect("exit-closes-fds-1", (int32_t)vfs_open_count(1), 0);
    pass &= expect("exit-closes-fds-2", (int32_t)vfs_open_count(2), 0);

    /* The errno mapper itself: every FAT12 failure class must land on
     * exactly one VFS errno, unknown codes fall back to EIO. */
    pass &= expect("map-ok", fat12_map_errno(1), 0);
    pass &= expect("map-io", fat12_map_errno(-FAT12_ERR_IO), -VFS_EIO);
    pass &= expect("map-enoent",
                   fat12_map_errno(-FAT12_ERR_NOT_FOUND), -VFS_ENOENT);
    pass &= expect("map-eexist",
                   fat12_map_errno(-FAT12_ERR_EXISTS), -VFS_EEXIST);
    pass &= expect("map-enospc",
                   fat12_map_errno(-FAT12_ERR_NO_SPACE), -VFS_ENOSPC);
    pass &= expect("map-einval",
                   fat12_map_errno(-FAT12_ERR_INVALID), -VFS_EINVAL);
    pass &= expect("map-unknown", fat12_map_errno(-99), -VFS_EIO);

    /* The interface itself: unknown filesystems and null vectors are
     * rejected without disturbing the mounted root, which must keep
     * serving descriptors afterwards. */
    pass &= expect("if-bad-mount", vfs_mount_root("no-such-fs"),
                   -VFS_EINVAL);
    pass &= expect("if-null-register", vfs_register_fs(0), 0);
    fd = vfs_open(FS_TEST_PID, "HAKU.ELF", VFS_O_RDONLY);
    pass &= expect("if-root-alive", fd >= 3 ? 0 : fd, 0);
    if (fd >= 3) vfs_close(FS_TEST_PID, (uint32_t)fd);
    else pass = 0;

    /* Missing file. */
    pass &= expect("missing-open",
                   vfs_open(FS_TEST_PID, "NOPE.TXT", VFS_O_RDONLY),
                   -VFS_ENOENT);

    /* Invalid input: bad flag word, TRUNC without write, EXCL alone. */
    pass &= expect("invalid-flags",
                   vfs_open(FS_TEST_PID, "HAKU.ELF", 0xFFFFu), -VFS_EINVAL);
    pass &= expect("invalid-trunc",
                   vfs_open(FS_TEST_PID, "HAKU.ELF",
                            VFS_O_RDONLY | VFS_O_TRUNC),
                   -VFS_EINVAL);
    pass &= expect("invalid-excl",
                   vfs_open(FS_TEST_PID, "HAKU.ELF", VFS_O_EXCL),
                   -VFS_EINVAL);

    /* Existing file with O_CREAT|O_EXCL. */
    pass &= expect("exists",
                   vfs_open(FS_TEST_PID, "HAKU.ELF",
                            VFS_O_CREAT | VFS_O_EXCL | VFS_O_RDWR),
                   -VFS_EEXIST);

    /* Bad descriptors, including NULL-buffer I/O (invalid, not I/O). */
    pass &= expect("badfd-read",
                   vfs_read(FS_TEST_PID, 99, scratch, 1), -VFS_EBADF);
    pass &= expect("badfd-write",
                   vfs_write(FS_TEST_PID, 99, scratch, 1), -VFS_EBADF);
    pass &= expect("badfd-close", vfs_close(FS_TEST_PID, 99), -VFS_EBADF);
    pass &= expect("badfd-seek",
                   vfs_seek(FS_TEST_PID, 99, 0, 0), -VFS_EBADF);

    /* Wrong-mode descriptor: write/seek on a read-only handle. */
    fd = vfs_open(FS_TEST_PID, "HAKU.ELF", VFS_O_RDONLY);
    pass &= expect("readonly-open-fd", fd >= 3 ? 0 : fd, 0);
    if (fd >= 3) {
        pass &= expect("readonly-write",
                       vfs_write(FS_TEST_PID, (uint32_t)fd, scratch, 1),
                       -VFS_EBADF);
        pass &= expect("null-read",
                       vfs_read(FS_TEST_PID, (uint32_t)fd, 0, 1),
                       -VFS_EINVAL);
        pass &= expect("bad-whence",
                       vfs_seek(FS_TEST_PID, (uint32_t)fd, 0, 99),
                       -VFS_EINVAL);
        pass &= expect("close-ok",
                       vfs_close(FS_TEST_PID, (uint32_t)fd), 0);
    } else {
        pass = 0;
    }

    if (pass) {
        klog_prefix();
        klog_write("FS-ERRORS-OK: enoent/einval/eexist/ebadf/eio-map distinct\n");
    } else {
        klog_prefix();
        klog_write("FS-ERRORS-FAIL\n");
    }

    /* Read-only mount: EROFS on every mutating path, reads unaffected. */
    if (test_read_only()) {
        klog_prefix();
        klog_write("FS-EROFS-OK: read-only mount rejects writes\n");
    } else {
        klog_prefix();
        klog_write("FS-EROFS-FAIL\n");
    }

    /* Empty files and exact/crossing sector boundaries. */
    if (test_empty_file() & test_sector_edges()) {
        klog_prefix();
        klog_write("FS-FILE-EDGE-OK: empty/exact-sector/span/partial\n");
    } else {
        klog_prefix();
        klog_write("FS-EDGE-FAIL\n");
    }

    /* Injected FAT faults (each restored): cut, bad, and reserved
     * chain values must read as EIO; cycles must terminate; a FAT with
     * no free cluster must report ENOSPC on write. */
    if (test_corrupt_chains()) {
        klog_prefix();
        klog_write("FS-CORRUPT-OK: trunc/bad/reserved/cycle/datafull\n");
    } else {
        klog_prefix();
        klog_write("FS-CORRUPT-FAIL\n");
    }

    /* ATA transfer validation. */
    if (test_ata_errors()) {
        klog_prefix();
        klog_write("FS-ATA-OK: bounds/null/zero-sector\n");
    } else {
        klog_prefix();
        klog_write("FS-ATA-FAIL\n");
    }

    /* Metadata behind stat/fstat: type bits, size, link count, block
     * size, and the 512-byte block count the Linux stat layouts carry. */
    if (test_stat_metadata()) {
        klog_prefix();
        klog_write("FS-STAT-OK: mode/size/nlink/blksize/blocks\n");
    } else {
        klog_prefix();
        klog_write("FS-STAT-FAIL\n");
    }

    /* Failed-write cleanup: sizes only cover durable bytes, short
     * writes keep their prefix, and failed metadata leaves no
     * unreachable clusters behind. */
    if (test_write_rollback()) {
        klog_prefix();
        klog_write("FS-ROLLBACK-OK: gap/partial/trunc cleanup\n");
    } else {
        klog_prefix();
        klog_write("FS-ROLLBACK-FAIL\n");
    }

    /* Full volume: create files until the root directory refuses with
     * ENOSPC. Self-calibrating (no hardcoded entry count); the filler
     * entries are harmless because every user test already finished. */
    {
        uint32_t created = 0;
        int full = 0;
        for (uint32_t i = 0; i < FS_FILL_MAX; i++) {
            char name[12];
            int32_t result;
            fill_name(name, i);
            result = vfs_open(FS_TEST_PID, name,
                              VFS_O_CREAT | VFS_O_RDWR);
            if (result == -VFS_ENOSPC) {
                full = 1;
                break;
            }
            if (result < 3) {
                klog_prefix();
                klog_write("FS-ENOSPC-FAIL: fill open got=");
                klog_write(fat12_error_name(result));
                klog_write("\n");
                break;
            }
            vfs_close(FS_TEST_PID, (uint32_t)result);
            created++;
        }
        if (full) {
            klog_prefix();
            klog_write("FS-ENOSPC-OK: root-full files=");
            klog_dec(created);
            klog_write("\n");
        } else if (pass) {
            klog_prefix();
            klog_write("FS-ENOSPC-FAIL: volume never reported full\n");
        }
    }
}
