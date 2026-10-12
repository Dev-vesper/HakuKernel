#include "vfs.h"
#include "fat12.h"
#include "idt.h"
#include "scheduler.h"

#define VFS_MAX_PROCESSES SCHEDULER_MAX_PROCESSES
#define VFS_FD_COUNT 8u
#define VFS_FIRST_FILE_FD 3u
#define VFS_SUPPORTED_FLAGS \
    (VFS_O_ACCMODE | VFS_O_CREAT | VFS_O_EXCL | VFS_O_TRUNC | VFS_O_APPEND)

typedef struct {
    fat12_file_t file;
    int used;
} vfs_fd_t;

static vfs_fd_t descriptors[VFS_MAX_PROCESSES][VFS_FD_COUNT];

static int pid_valid(uint32_t pid) {
    return pid > 0 && pid <= VFS_MAX_PROCESSES;
}

void vfs_init(void) {
    memset(descriptors, 0, sizeof(descriptors));
}

int32_t vfs_open(uint32_t pid, const char *path, uint32_t flags) {
    uint8_t fat_flags = 0;
    uint32_t fd;
    int open_result;
    uint32_t irq_flags;
    if (!pid_valid(pid) || !path || (flags & ~VFS_SUPPORTED_FLAGS) ||
        (flags & VFS_O_ACCMODE) == VFS_O_ACCMODE ||
        ((flags & VFS_O_EXCL) && !(flags & VFS_O_CREAT)) ||
        ((flags & VFS_O_TRUNC) &&
         (flags & VFS_O_ACCMODE) == VFS_O_RDONLY))
        return -VFS_EINVAL;
    if ((flags & VFS_O_ACCMODE) != VFS_O_WRONLY)
        fat_flags |= FAT12_OPEN_READ;
    if ((flags & VFS_O_ACCMODE) != VFS_O_RDONLY)
        fat_flags |= FAT12_OPEN_WRITE;
    if (flags & VFS_O_CREAT) fat_flags |= FAT12_OPEN_CREATE;
    if (flags & VFS_O_EXCL) fat_flags |= FAT12_OPEN_EXCL;
    if (flags & VFS_O_TRUNC) fat_flags |= FAT12_OPEN_TRUNC;
    if (flags & VFS_O_APPEND) fat_flags |= FAT12_OPEN_APPEND;

    for (fd = VFS_FIRST_FILE_FD; fd < VFS_FD_COUNT; fd++)
        if (!descriptors[pid - 1u][fd].used) break;
    if (fd == VFS_FD_COUNT) return -VFS_EMFILE;

    irq_flags = cpu_irq_save();
    open_result = fat12_file_open(path, fat_flags,
                                  &descriptors[pid - 1u][fd].file);
    cpu_irq_restore(irq_flags);
    if (open_result != 1) {
        if (open_result == -FAT12_ERR_NOT_FOUND) return -VFS_ENOENT;
        if (open_result == -FAT12_ERR_EXISTS) return -VFS_EEXIST;
        if (open_result == -FAT12_ERR_NO_SPACE) return -VFS_ENOSPC;
        if (open_result == -FAT12_ERR_INVALID) return -VFS_EINVAL;
        return -VFS_EIO;
    }
    descriptors[pid - 1u][fd].used = 1;
    return (int32_t)fd;
}

int32_t vfs_close(uint32_t pid, uint32_t fd) {
    vfs_fd_t *descriptor;
    uint32_t irq_flags;
    if (!pid_valid(pid) || fd < VFS_FIRST_FILE_FD || fd >= VFS_FD_COUNT)
        return -VFS_EBADF;
    descriptor = &descriptors[pid - 1u][fd];
    if (!descriptor->used) return -VFS_EBADF;
    irq_flags = cpu_irq_save();
    if (!fat12_file_close(&descriptor->file)) {
        cpu_irq_restore(irq_flags);
        return -VFS_EIO;
    }
    cpu_irq_restore(irq_flags);
    memset(descriptor, 0, sizeof(*descriptor));
    return 0;
}

int32_t vfs_read(uint32_t pid, uint32_t fd, void *buffer, uint32_t count) {
    vfs_fd_t *descriptor;
    uint32_t copied = 0;
    uint32_t irq_flags;
    int ok;
    if (!pid_valid(pid) || fd < VFS_FIRST_FILE_FD || fd >= VFS_FD_COUNT)
        return -VFS_EBADF;
    descriptor = &descriptors[pid - 1u][fd];
    if (!descriptor->used) return -VFS_EBADF;
    if (!(descriptor->file.flags & FAT12_OPEN_READ)) return -VFS_EBADF;
    irq_flags = cpu_irq_save();
    ok = fat12_file_read(&descriptor->file, buffer, count, &copied);
    cpu_irq_restore(irq_flags);
    if (copied) return (int32_t)copied;
    return ok ? 0 : -VFS_EIO;
}

int32_t vfs_write(uint32_t pid, uint32_t fd, const void *buffer,
                  uint32_t count) {
    vfs_fd_t *descriptor;
    uint32_t written = 0;
    uint32_t irq_flags;
    int ok;
    if (!pid_valid(pid) || fd < VFS_FIRST_FILE_FD || fd >= VFS_FD_COUNT)
        return -VFS_EBADF;
    descriptor = &descriptors[pid - 1u][fd];
    if (!descriptor->used) return -VFS_EBADF;
    if (!(descriptor->file.flags & FAT12_OPEN_WRITE)) return -VFS_EBADF;
    irq_flags = cpu_irq_save();
    ok = fat12_file_write(&descriptor->file, buffer, count, &written);
    cpu_irq_restore(irq_flags);
    if (written) return (int32_t)written;
    return ok ? 0 : -VFS_EIO;
}

int32_t vfs_seek(uint32_t pid, uint32_t fd, int32_t offset,
                 uint32_t whence) {
    vfs_fd_t *descriptor;
    uint32_t position;
    uint32_t irq_flags;
    int ok;
    if (!pid_valid(pid) || fd < VFS_FIRST_FILE_FD || fd >= VFS_FD_COUNT)
        return -VFS_EBADF;
    descriptor = &descriptors[pid - 1u][fd];
    if (!descriptor->used) return -VFS_EBADF;
    irq_flags = cpu_irq_save();
    ok = fat12_file_seek(&descriptor->file, offset, whence, &position);
    cpu_irq_restore(irq_flags);
    if (!ok || position > 0x7FFFFFFFu) return -VFS_EINVAL;
    return (int32_t)position;
}

void vfs_close_all(uint32_t pid) {
    if (!pid_valid(pid)) return;
    for (uint32_t fd = VFS_FIRST_FILE_FD; fd < VFS_FD_COUNT; fd++) {
        vfs_fd_t *descriptor = &descriptors[pid - 1u][fd];
        if (!descriptor->used) continue;
        fat12_file_close(&descriptor->file);
        memset(descriptor, 0, sizeof(*descriptor));
    }
}
