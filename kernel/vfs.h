#ifndef HAKU_VFS_H
#define HAKU_VFS_H

#include "cpu.h"

#define VFS_O_RDONLY 0x0000u
#define VFS_O_WRONLY 0x0001u
#define VFS_O_RDWR   0x0002u
#define VFS_O_ACCMODE 0x0003u
#define VFS_O_CREAT  0x0040u
#define VFS_O_EXCL   0x0080u
#define VFS_O_TRUNC  0x0200u
#define VFS_O_APPEND 0x0400u

#define VFS_EIO            5
#define VFS_ENOENT         2
#define VFS_EBADF          9
#define VFS_EEXIST         17
#define VFS_EINVAL         22
#define VFS_EMFILE         24
#define VFS_ENOSPC         28

void vfs_init(void);
/* Translate a fat12_* result (1 or negative -FAT12_ERR_*) into 0 or a
 * negative -VFS_E* errno. Unit-checked by the fs error self-test, so
 * every FAT12 failure class keeps one distinct errno end to end. */
int32_t vfs_map_fat_error(int fat_result);
int32_t vfs_open(uint32_t pid, const char *path, uint32_t flags);
int32_t vfs_close(uint32_t pid, uint32_t fd);
int32_t vfs_read(uint32_t pid, uint32_t fd, void *buffer, uint32_t count);
int32_t vfs_write(uint32_t pid, uint32_t fd, const void *buffer,
                  uint32_t count);
int32_t vfs_seek(uint32_t pid, uint32_t fd, int32_t offset, uint32_t whence);
void vfs_close_all(uint32_t pid);

#endif
