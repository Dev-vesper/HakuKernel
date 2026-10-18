/* FAT12 backend for the filesystem-independent VFS layer.
 *
 * This is the only module (besides the fault-injection self-test) that
 * knows both worlds: it translates VFS_O_* flags to FAT12_OPEN_* flags,
 * owns the per-open fat12_file_t handle (kmalloc/kfree), and maps every
 * -FAT12_ERR_* result to a -VFS_E* errno. VFS itself never includes
 * fat12.h; adding another filesystem means adding another module shaped
 * exactly like this one plus one register call at boot.
 */
#ifndef HAKU_FAT12_VFS_H
#define HAKU_FAT12_VFS_H

#include "cpu.h"
#include "vfs.h"

/* The ops vector handed to vfs_register_fs. */
extern const vfs_fs_ops_t fat12_fs_ops;

/* Translate a fat12_* result (1 or negative -FAT12_ERR_*) into 0 or a
 * negative -VFS_E* errno. Unit-checked by the fs error self-test, so
 * every FAT12 failure class keeps one distinct errno end to end. */
int32_t fat12_map_errno(int fat_result);

#endif
