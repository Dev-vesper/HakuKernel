#include "fs_test.h"
#include "console.h"
#include "fat12.h"
#include "scheduler.h"
#include "vfs.h"

#define FS_TEST_PID 1u
#define FS_FILL_MAX 600u

static int test_done;
static uint8_t scratch[16];

static void print_result(const char *label, int32_t value) {
    serial_write("FS-ERROR-FAIL ");
    serial_write(label);
    serial_write(" got=");
    if (value < 0) {
        serial_putc('-');
        serial_dec((uint32_t)(-value));
    } else {
        serial_dec((uint32_t)value);
    }
    serial_write(" want distinct errno\n");
}

static int expect(const char *label, int32_t got, int32_t want) {
    if (got == want) return 1;
    print_result(label, got);
    return 0;
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
