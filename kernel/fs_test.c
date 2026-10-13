#include "fs_test.h"
#include "ata.h"
#include "console.h"
#include "fat12.h"
#include "heap.h"
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
    serial_write(tag);
    serial_write(" ");
    serial_write(label);
    serial_write(" got=");
    if (got < 0) {
        serial_putc('-');
        serial_dec((uint32_t)(-got));
    } else {
        serial_dec((uint32_t)got);
    }
    serial_write("\n");
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

static int open_scratch_chain(fat12_file_t *file, uint16_t *first_out) {
    uint32_t written = 0;
    int opened;
    fill_pattern(edge_buf, 3u * FS_SECTOR);
    opened = fat12_file_open("CHAIN.TXT",
                             FAT12_OPEN_READ | FAT12_OPEN_WRITE |
                             FAT12_OPEN_CREATE | FAT12_OPEN_TRUNC,
                             file);
    if (opened != 1) return opened;
    if (fat12_file_write(file, edge_buf, 3u * FS_SECTOR, &written) != 1 ||
        written != 3u * FS_SECTOR) {
        fat12_file_close(file);
        return -FAT12_ERR_IO;
    }
    *first_out = file->first_cluster;
    return fat12_file_close(file);
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
    fat12_file_t setup;
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
    setup_result = open_scratch_chain(&setup, &first);
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
     * freeing the loop must fail cleanly instead of spinning. */
    memcpy(work, snap.image, snap.sectors * FS_SECTOR);
    chain_set(work, c, b);
    ok &= expect_tagged("FS-CORRUPT-FAIL", "cycle-patch",
                        snap_write_all(&snap, work), 1);
    ok &= heal_check("cycle-read-terminates", 0, 3u * FS_SECTOR);
    ok &= expect_tagged("FS-CORRUPT-FAIL", "cycle-trunc-eio",
                        vfs_open(FS_TEST_PID, "CHAIN.TXT",
                                 VFS_O_RDWR | VFS_O_TRUNC),
                        -VFS_EIO);
    ok &= expect_tagged("FS-CORRUPT-FAIL", "cycle-restore",
                        snap_write_all(&snap, snap.image), 1);
    ok &= heal_check("cycle-heal", 0, 3u * FS_SECTOR);

    /* T5: every data cluster marked used. Extending any file must
     * report a full disk instead of an I/O error. */
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

static int test_ata_errors(void) {
    int ok = 1;
    uint32_t total = ata_sector_count();
    ok &= expect_tagged("FS-ATA-FAIL", "ata-present", total != 0, 1);
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

void fs_error_self_test(void) {
    int32_t fd;
    int pass = 1;
    if (test_done || !scheduler_user_tasks_done()) return;
    test_done = 1;

    /* The errno mapper itself: every FAT12 failure class must land on
     * exactly one VFS errno, unknown codes fall back to EIO. */
    pass &= expect("map-ok", vfs_map_fat_error(1), 0);
    pass &= expect("map-io", vfs_map_fat_error(-FAT12_ERR_IO), -VFS_EIO);
    pass &= expect("map-enoent",
                   vfs_map_fat_error(-FAT12_ERR_NOT_FOUND), -VFS_ENOENT);
    pass &= expect("map-eexist",
                   vfs_map_fat_error(-FAT12_ERR_EXISTS), -VFS_EEXIST);
    pass &= expect("map-enospc",
                   vfs_map_fat_error(-FAT12_ERR_NO_SPACE), -VFS_ENOSPC);
    pass &= expect("map-einval",
                   vfs_map_fat_error(-FAT12_ERR_INVALID), -VFS_EINVAL);
    pass &= expect("map-unknown", vfs_map_fat_error(-99), -VFS_EIO);

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
        serial_write("FS-ERRORS-OK ");
        serial_write("enoent/einval/eexist/ebadf/eio-map distinct\n");
    } else {
        serial_write("FS-ERRORS-FAIL\n");
    }

    /* Empty files and exact/crossing sector boundaries. */
    if (test_empty_file() & test_sector_edges()) {
        serial_write("FS-FILE-EDGE-OK empty/exact-sector/span/partial\n");
    } else {
        serial_write("FS-EDGE-FAIL\n");
    }

    /* Injected FAT faults (each restored): cut, bad, and reserved
     * chain values must read as EIO; cycles must terminate; a FAT with
     * no free cluster must report ENOSPC on write. */
    if (test_corrupt_chains()) {
        serial_write("FS-CORRUPT-OK trunc/bad/reserved/cycle/datafull\n");
    } else {
        serial_write("FS-CORRUPT-FAIL\n");
    }

    /* ATA transfer validation. */
    if (test_ata_errors()) {
        serial_write("FS-ATA-OK bounds/null/zero-sector\n");
    } else {
        serial_write("FS-ATA-FAIL\n");
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
                serial_write("FS-ENOSPC-FAIL fill open got=");
                serial_write(fat12_error_name(result));
                serial_write("\n");
                break;
            }
            vfs_close(FS_TEST_PID, (uint32_t)result);
            created++;
        }
        if (full) {
            serial_write("FS-ENOSPC-OK root-full files=");
            serial_dec(created);
            serial_write("\n");
        } else if (pass) {
            serial_write("FS-ENOSPC-FAIL volume never reported full\n");
        }
    }
}
