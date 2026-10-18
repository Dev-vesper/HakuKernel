#ifndef HAKU_VFS_H
#define HAKU_VFS_H

#include "cpu.h"
#include "haku/abi.h"

#define VFS_O_RDONLY  HAKU_O_RDONLY
#define VFS_O_WRONLY  HAKU_O_WRONLY
#define VFS_O_RDWR    HAKU_O_RDWR
#define VFS_O_ACCMODE HAKU_O_ACCMODE
#define VFS_O_CREAT   HAKU_O_CREAT
#define VFS_O_EXCL    HAKU_O_EXCL
#define VFS_O_TRUNC   HAKU_O_TRUNC
#define VFS_O_APPEND  HAKU_O_APPEND

#define VFS_EIO    HAKU_EIO
#define VFS_ENOENT HAKU_ENOENT
#define VFS_EBADF  HAKU_EBADF
#define VFS_EEXIST HAKU_EEXIST
#define VFS_EINVAL HAKU_EINVAL
#define VFS_EMFILE HAKU_EMFILE
#define VFS_ENOSPC HAKU_ENOSPC
#define VFS_EROFS  HAKU_EROFS
#define VFS_ENOTDIR HAKU_ENOTDIR
#define VFS_EISDIR  HAKU_EISDIR
#define VFS_ENOTEMPTY HAKU_ENOTEMPTY

/* File types for vfs_stat_t.mode. */
#define VFS_S_IFREG HAKU_S_IFREG
#define VFS_S_IFDIR HAKU_S_IFDIR

/* Longest path (including NUL) the VFS and its backends accept. */
#define VFS_PATH_MAX 64u

/* Minimal metadata: byte size plus mode. Backends fill size; VFS callers
 * must treat every other attribute as unsupported. */
typedef haku_stat_t vfs_stat_t;

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
    int32_t (*unlink)(const char *path);
    int32_t (*rename)(const char *oldpath, const char *newpath);
    int32_t (*mkdir)(const char *path);
    int32_t (*rmdir)(const char *path);
    int32_t (*stat)(const char *path, vfs_stat_t *out);
    int32_t (*fstat)(void *handle, vfs_stat_t *out);
} vfs_fs_ops_t;

/* Character-device backend for the standard descriptors 0/1/2. These are
 * not path-openable: VFS installs one for stdio and dup/dup2 may redirect
 * it. read/write return 1 or a negative -VFS_E* errno and report the
 * transferred byte count via out_count; a device may block (console input
 * does) so VFS calls them without masking interrupts. Devices do not seek
 * or stat, so those operations fail with EINVAL. */
typedef struct vfs_device_ops {
    const char *name;
    int32_t (*read)(void *buffer, uint32_t count, uint32_t *out_count);
    int32_t (*write)(const void *buffer, uint32_t count,
                     uint32_t *out_count);
} vfs_device_ops_t;

/* Boot-time assembly: backends register here, then one is mounted as the
 * root filesystem that all descriptor operations route to. register
 * returns 1/0; mount_root returns 1 or a negative -VFS_E* errno.
 * VFS_MOUNT_RDONLY mounts the root read-only: reads and metadata still
 * work, while create/write/truncate/unlink/rename/mkdir/rmdir return
 * -VFS_EROFS. An existing mount keeps serving if the new mount fails. */
#define VFS_MOUNT_RDONLY 0x1u
int vfs_register_fs(const vfs_fs_ops_t *ops);
int32_t vfs_mount_root(const char *name);
int32_t vfs_mount_root_flags(const char *name, uint32_t flags);
/* Install a character device (the console) for the standard descriptors. */
int vfs_register_device(const vfs_device_ops_t *ops);

void vfs_init(void);
/* Create the standard descriptors 0 (read console), 1 and 2 (write
 * console) for a process. Called when a process is created; spawned
 * children instead inherit their parent's table (see vfs_inherit). */
int vfs_open_stdio(uint32_t pid);
/* Copy the parent's descriptor table into the child, sharing the open
 * file descriptions (each copy bumps the reference count). Called on
 * spawn before the child runs. Returns 1 or a negative -VFS_E* errno. */
int32_t vfs_inherit(uint32_t parent_pid, uint32_t child_pid);
/* Number of descriptors currently open for a process (0..VFS_FD_COUNT). */
uint32_t vfs_open_count(uint32_t pid);
int32_t vfs_open(uint32_t pid, const char *path, uint32_t flags);
int32_t vfs_close(uint32_t pid, uint32_t fd);
int32_t vfs_read(uint32_t pid, uint32_t fd, void *buffer, uint32_t count);
int32_t vfs_write(uint32_t pid, uint32_t fd, const void *buffer,
                  uint32_t count);
int32_t vfs_seek(uint32_t pid, uint32_t fd, int32_t offset, uint32_t whence);
/* dup/dup2 share the open file description behind a descriptor: the copy
 * sees the same offset/access mode and keeps the file alive until the last
 * of them closes. dup returns the lowest free fd, dup2 returns newfd (or
 * closes and reuses it if already open); both return 1 or -VFS_E*. Any fd
 * in 0..VFS_FD_COUNT-1 is valid, including the standard descriptors, so
 * dup2 can redirect 0/1/2 onto a file or another handle. */
int32_t vfs_dup(uint32_t pid, uint32_t fd);
int32_t vfs_dup2(uint32_t pid, uint32_t oldfd, uint32_t newfd);
/* Namespace and metadata operations. Unlink removes the name at once;
 * descriptors open on the file keep working until closed (clusters are
 * released at the last such close). Rename repoints open descriptors to
 * the new entry, so they keep working too. All return 1 or -VFS_E*. */
int32_t vfs_unlink(uint32_t pid, const char *path);
int32_t vfs_rename(uint32_t pid, const char *oldpath, const char *newpath);
int32_t vfs_mkdir(uint32_t pid, const char *path);
int32_t vfs_rmdir(uint32_t pid, const char *path);
int32_t vfs_stat(uint32_t pid, const char *path, vfs_stat_t *out);
int32_t vfs_fstat(uint32_t pid, uint32_t fd, vfs_stat_t *out);
void vfs_close_all(uint32_t pid);

#endif
