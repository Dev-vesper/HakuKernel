/* FAT12 backend: vfs_fs_ops over kernel/fat12.c. All ops return 1 on
 * success or a negative -VFS_E* errno, per the interface contract. */
#include "fat12_vfs.h"
#include "fat12.h"
#include "heap.h"

int32_t fat12_map_errno(int fat_result) {
    if (fat_result == 1) return 0;
    if (fat_result == -FAT12_ERR_NOT_FOUND) return -VFS_ENOENT;
    if (fat_result == -FAT12_ERR_EXISTS) return -VFS_EEXIST;
    if (fat_result == -FAT12_ERR_NO_SPACE) return -VFS_ENOSPC;
    if (fat_result == -FAT12_ERR_INVALID) return -VFS_EINVAL;
    return -VFS_EIO;
}

static uint8_t vfs_to_fat_flags(uint32_t vfs_flags) {
    uint8_t fat_flags = 0;
    if ((vfs_flags & VFS_O_ACCMODE) != VFS_O_WRONLY)
        fat_flags |= FAT12_OPEN_READ;
    if ((vfs_flags & VFS_O_ACCMODE) != VFS_O_RDONLY)
        fat_flags |= FAT12_OPEN_WRITE;
    if (vfs_flags & VFS_O_CREAT) fat_flags |= FAT12_OPEN_CREATE;
    if (vfs_flags & VFS_O_EXCL) fat_flags |= FAT12_OPEN_EXCL;
    if (vfs_flags & VFS_O_TRUNC) fat_flags |= FAT12_OPEN_TRUNC;
    if (vfs_flags & VFS_O_APPEND) fat_flags |= FAT12_OPEN_APPEND;
    return fat_flags;
}

static int32_t fat12_vfs_mount(void) {
    int mounted = fat12_mount();
    if (mounted == 1) return 1;
    return fat12_map_errno(mounted);
}

static int32_t fat12_vfs_open(const char *path, uint32_t flags,
                              void **handle_out) {
    fat12_file_t *file;
    int opened;
    if (!handle_out) return -VFS_EINVAL;
    *handle_out = 0;
    file = (fat12_file_t *)kmalloc(sizeof(fat12_file_t));
    if (!file) return -VFS_ENOSPC;
    opened = fat12_file_open(path, vfs_to_fat_flags(flags), file);
    if (opened != 1) {
        kfree(file);
        return fat12_map_errno(opened);
    }
    *handle_out = file;
    return 1;
}

static int32_t fat12_vfs_close(void *handle) {
    int closed;
    if (!handle) return -VFS_EINVAL;
    closed = fat12_file_close((fat12_file_t *)handle);
    kfree(handle);
    if (closed != 1) return fat12_map_errno(closed);
    return 1;
}

static int32_t fat12_vfs_read(void *handle, void *buffer, uint32_t count,
                              uint32_t *out_count) {
    uint32_t copied = 0;
    int done;
    if (!out_count) return -VFS_EINVAL;
    *out_count = 0;
    done = fat12_file_read((fat12_file_t *)handle, buffer, count, &copied);
    *out_count = copied;
    if (done == 1) return 1;
    return fat12_map_errno(done);
}

static int32_t fat12_vfs_write(void *handle, const void *buffer,
                               uint32_t count, uint32_t *out_count) {
    uint32_t written = 0;
    int done;
    if (!out_count) return -VFS_EINVAL;
    *out_count = 0;
    done = fat12_file_write((fat12_file_t *)handle, buffer, count, &written);
    *out_count = written;
    if (done == 1) return 1;
    return fat12_map_errno(done);
}

static int32_t fat12_vfs_seek(void *handle, int32_t offset, uint32_t whence,
                              uint32_t *out_pos) {
    uint32_t position = 0;
    int moved;
    if (!out_pos) return -VFS_EINVAL;
    *out_pos = 0;
    moved = fat12_file_seek((fat12_file_t *)handle, offset, whence,
                            &position);
    *out_pos = position;
    if (moved == 1) return 1;
    return fat12_map_errno(moved);
}

const vfs_fs_ops_t fat12_fs_ops = {
    "fat12",
    fat12_vfs_mount,
    fat12_vfs_open,
    fat12_vfs_close,
    fat12_vfs_read,
    fat12_vfs_write,
    fat12_vfs_seek,
};
