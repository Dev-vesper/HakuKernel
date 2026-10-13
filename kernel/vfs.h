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

/* Filesystem-independent backend interface (Linux file_operations style,
 * scaled to this kernel). VFS owns the per-process descriptor tables and
 * only ever calls through these pointers; it never names a concrete
 * filesystem. Every op returns 1 on success or a negative -VFS_E* errno;
 * read/write additionally report the transferred byte count via out_count
 * (a short count with return 1 is a durable partial transfer, never an
 * error). Backends allocate their per-open handle themselves (kmalloc)
 * and release it in close; VFS only stores the pointer. */
typedef struct vfs_fs_ops {
    const char *name;
    int32_t (*mount)(void);
    int32_t (*open)(const char *path, uint32_t flags, void **handle_out);
    int32_t (*close)(void *handle);
    int32_t (*read)(void *handle, void *buffer, uint32_t count,
                    uint32_t *out_count);
    int32_t (*write)(void *handle, const void *buffer, uint32_t count,
                     uint32_t *out_count);
    int32_t (*seek)(void *handle, int32_t offset, uint32_t whence,
                    uint32_t *out_pos);
} vfs_fs_ops_t;

/* Boot-time assembly: backends register here, then one is mounted as the
 * root filesystem that all descriptor operations route to. register
 * returns 1/0; mount_root returns 1 or a negative -VFS_E* errno. */
int vfs_register_fs(const vfs_fs_ops_t *ops);
int32_t vfs_mount_root(const char *name);

void vfs_init(void);
int32_t vfs_open(uint32_t pid, const char *path, uint32_t flags);
int32_t vfs_close(uint32_t pid, uint32_t fd);
int32_t vfs_read(uint32_t pid, uint32_t fd, void *buffer, uint32_t count);
int32_t vfs_write(uint32_t pid, uint32_t fd, const void *buffer,
                  uint32_t count);
int32_t vfs_seek(uint32_t pid, uint32_t fd, int32_t offset, uint32_t whence);
void vfs_close_all(uint32_t pid);

#endif
