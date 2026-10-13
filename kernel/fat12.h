#ifndef HAKU_FAT12_H
#define HAKU_FAT12_H

#include "cpu.h"

typedef struct {
    uint8_t short_name[11];
    uint16_t first_cluster;
    uint32_t size;
    uint32_t position;
    uint8_t flags;
    int opened;
    /* Directory location of this file's entry, captured at open: refresh
     * and metadata writes verify the slot instead of re-finding by name,
     * so rename repoints handles and unlink detaches them precisely.
     * unlinked sticks once set: the name is gone even if the slot is
     * later reused by another file. */
    uint32_t dir_lba;
    uint32_t dir_off;
    int unlinked;
} fat12_file_t;

/* Minimal file metadata for stat/fstat (backend maps size to vfs_stat_t;
 * every entry here is a regular file, there are no directories). */
typedef struct {
    uint32_t size;
} fat12_stat_t;

#define FAT12_OPEN_READ   0x01u
#define FAT12_OPEN_WRITE  0x02u
#define FAT12_OPEN_CREATE 0x04u
#define FAT12_OPEN_TRUNC  0x08u
#define FAT12_OPEN_APPEND 0x10u
#define FAT12_OPEN_EXCL   0x20u

#define FAT12_ERR_IO        1
#define FAT12_ERR_NOT_FOUND 2
#define FAT12_ERR_EXISTS    3
#define FAT12_ERR_NO_SPACE  4
#define FAT12_ERR_INVALID   5

/* Return convention for every fat12_* call below: 1 on success, or a
 * negative -FAT12_ERR_* code naming the exact failure (disk I/O error,
 * missing entry, existing entry, full volume/directory, invalid
 * argument). Callers must compare against 1, never use !result. */
const char *fat12_error_name(int result);

/* Fault-injection hooks for the filesystem self-test. Sector access is
 * bounds-checked against the mounted volume; geometry inputs are
 * validated before use. Same 1 / -FAT12_ERR_* convention. */
int fat12_test_sector_info(uint32_t *fat_start, uint32_t *fat_sectors,
                           uint32_t *fat_count, uint32_t *cluster_count,
                           uint32_t *sectors_per_cluster);
int fat12_test_read_sector(uint32_t lba, void *buffer);
int fat12_test_write_sector(uint32_t lba, const void *buffer);
/* Read-only introspection for the fault-injection self-test: report the
 * first data cluster of a root-directory file without opening it, so the
 * test can build file content through the public VFS interface and still
 * locate the chain to corrupt. Same 1 / -FAT12_ERR_* convention. */
int fat12_debug_first_cluster(const char *name, uint16_t *first_out);

int fat12_mount(void);
int fat12_read_file(const char *name, void *buffer, uint32_t capacity,
                    uint32_t *file_size);
int fat12_file_open(const char *name, uint8_t flags, fat12_file_t *file);
int fat12_file_close(fat12_file_t *file);
int fat12_file_read(fat12_file_t *file, void *buffer, uint32_t count,
                    uint32_t *read_count);
int fat12_file_write(fat12_file_t *file, const void *buffer, uint32_t count,
                     uint32_t *write_count);
int fat12_file_seek(fat12_file_t *file, int32_t offset, uint32_t whence,
                    uint32_t *new_position);
/* Remove a name: the entry is marked deleted immediately; clusters are
 * freed at once when no open handle references it, otherwise at the last
 * such close. Renaming to an existing name fails with EXISTS; renaming
 * a name onto itself succeeds as a no-op. Same 1 / -FAT12_ERR_*
 * convention. */
int fat12_file_unlink(const char *name);
int fat12_file_rename(const char *oldname, const char *newname);
int fat12_file_stat(const char *name, fat12_stat_t *out);
int fat12_file_fstat(fat12_file_t *file, fat12_stat_t *out);

#endif
