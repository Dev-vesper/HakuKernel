#include "vfs.h"
#include "idt.h"
#include "scheduler.h"

#define VFS_MAX_PROCESSES SCHEDULER_MAX_PROCESSES
#define VFS_FD_COUNT 8u
#define VFS_FIRST_FILE_FD 3u
#define VFS_MAX_FS 4u
#define VFS_SUPPORTED_FLAGS \
    (VFS_O_ACCMODE | VFS_O_CREAT | VFS_O_EXCL | VFS_O_TRUNC | VFS_O_APPEND)

typedef struct {
    const vfs_fs_ops_t *ops;
    uint32_t accmode;   /* VFS_O_RDONLY/WRONLY/RDWR, fixed at open */
    void *backend;      /* backend-allocated per-open handle */
    int used;
} vfs_fd_t;

static vfs_fd_t descriptors[VFS_MAX_PROCESSES][VFS_FD_COUNT];
static const vfs_fs_ops_t *fs_table[VFS_MAX_FS];
static uint32_t fs_count;
static const vfs_fs_ops_t *root_fs;

static int pid_valid(uint32_t pid) {
    return pid > 0 && pid <= VFS_MAX_PROCESSES;
}

static int name_equal(const char *a, const char *b) {
    size_t i = 0;
    for (;;) {
        if (a[i] != b[i]) return 0;
        if (a[i] == 0) return 1;
        i++;
    }
}

int vfs_register_fs(const vfs_fs_ops_t *ops) {
    if (!ops || !ops->name || !ops->mount || !ops->open || !ops->close ||
        !ops->read || !ops->write || !ops->seek || !ops->unlink ||
        !ops->rename || !ops->stat || !ops->fstat)
        return 0;
    for (uint32_t i = 0; i < fs_count; i++) {
        if (fs_table[i] == ops || name_equal(fs_table[i]->name, ops->name))
            return 0;
    }
    if (fs_count >= VFS_MAX_FS) return 0;
    fs_table[fs_count++] = ops;
    return 1;
}

int32_t vfs_mount_root(const char *name) {
    const vfs_fs_ops_t *ops = 0;
    int32_t mounted;
    if (!name) return -VFS_EINVAL;
    for (uint32_t i = 0; i < fs_count; i++) {
        if (name_equal(fs_table[i]->name, name)) {
            ops = fs_table[i];
            break;
        }
    }
    if (!ops) return -VFS_EINVAL;
    mounted = ops->mount();
    if (mounted != 1) return mounted;
    root_fs = ops;
    return 1;
}

void vfs_init(void) {
    memset(descriptors, 0, sizeof(descriptors));
    fs_count = 0;
    root_fs = 0;
}

int32_t vfs_open(uint32_t pid, const char *path, uint32_t flags) {
    vfs_fd_t *slot;
    uint32_t fd;
    void *handle = 0;
    int32_t opened;
    uint32_t irq_flags;
    if (!pid_valid(pid) || !path || (flags & ~VFS_SUPPORTED_FLAGS) ||
        (flags & VFS_O_ACCMODE) == VFS_O_ACCMODE ||
        ((flags & VFS_O_EXCL) && !(flags & VFS_O_CREAT)) ||
        ((flags & VFS_O_TRUNC) &&
         (flags & VFS_O_ACCMODE) == VFS_O_RDONLY))
        return -VFS_EINVAL;
    if (!root_fs) return -VFS_EIO;
    for (fd = VFS_FIRST_FILE_FD; fd < VFS_FD_COUNT; fd++)
        if (!descriptors[pid - 1u][fd].used) break;
    if (fd == VFS_FD_COUNT) return -VFS_EMFILE;

    irq_flags = cpu_irq_save();
    opened = root_fs->open(path, flags, &handle);
    cpu_irq_restore(irq_flags);
    if (opened != 1) return opened;
    slot = &descriptors[pid - 1u][fd];
    slot->ops = root_fs;
    slot->accmode = flags & VFS_O_ACCMODE;
    slot->backend = handle;
    slot->used = 1;
    return (int32_t)fd;
}

int32_t vfs_close(uint32_t pid, uint32_t fd) {
    vfs_fd_t *descriptor;
    uint32_t irq_flags;
    int32_t closed;
    if (!pid_valid(pid) || fd < VFS_FIRST_FILE_FD || fd >= VFS_FD_COUNT)
        return -VFS_EBADF;
    descriptor = &descriptors[pid - 1u][fd];
    if (!descriptor->used) return -VFS_EBADF;
    irq_flags = cpu_irq_save();
    closed = descriptor->ops->close(descriptor->backend);
    cpu_irq_restore(irq_flags);
    /* Linux detach-on-close: the descriptor is gone even when the
     * backend reports an error; the errno still reaches the caller. */
    memset(descriptor, 0, sizeof(*descriptor));
    return closed == 1 ? 0 : closed;
}

int32_t vfs_read(uint32_t pid, uint32_t fd, void *buffer, uint32_t count) {
    vfs_fd_t *descriptor;
    uint32_t copied = 0;
    uint32_t irq_flags;
    int32_t read_result;
    if (!pid_valid(pid) || fd < VFS_FIRST_FILE_FD || fd >= VFS_FD_COUNT)
        return -VFS_EBADF;
    descriptor = &descriptors[pid - 1u][fd];
    if (!descriptor->used) return -VFS_EBADF;
    if (descriptor->accmode == VFS_O_WRONLY) return -VFS_EBADF;
    if (!buffer && count) return -VFS_EINVAL;
    irq_flags = cpu_irq_save();
    read_result = descriptor->ops->read(descriptor->backend, buffer, count,
                                        &copied);
    cpu_irq_restore(irq_flags);
    if (copied) return (int32_t)copied;
    if (read_result == 1) return 0;
    return read_result;
}

int32_t vfs_write(uint32_t pid, uint32_t fd, const void *buffer,
                  uint32_t count) {
    vfs_fd_t *descriptor;
    uint32_t written = 0;
    uint32_t irq_flags;
    int32_t write_result;
    if (!pid_valid(pid) || fd < VFS_FIRST_FILE_FD || fd >= VFS_FD_COUNT)
        return -VFS_EBADF;
    descriptor = &descriptors[pid - 1u][fd];
    if (!descriptor->used) return -VFS_EBADF;
    if (descriptor->accmode == VFS_O_RDONLY) return -VFS_EBADF;
    if (!buffer && count) return -VFS_EINVAL;
    irq_flags = cpu_irq_save();
    write_result = descriptor->ops->write(descriptor->backend, buffer, count,
                                          &written);
    cpu_irq_restore(irq_flags);
    if (written) return (int32_t)written;
    if (write_result == 1) return 0;
    return write_result;
}

int32_t vfs_seek(uint32_t pid, uint32_t fd, int32_t offset,
                 uint32_t whence) {
    vfs_fd_t *descriptor;
    uint32_t position = 0;
    uint32_t irq_flags;
    int32_t moved;
    if (!pid_valid(pid) || fd < VFS_FIRST_FILE_FD || fd >= VFS_FD_COUNT)
        return -VFS_EBADF;
    descriptor = &descriptors[pid - 1u][fd];
    if (!descriptor->used) return -VFS_EBADF;
    irq_flags = cpu_irq_save();
    moved = descriptor->ops->seek(descriptor->backend, offset, whence,
                                  &position);
    cpu_irq_restore(irq_flags);
    if (moved != 1) return moved;
    if (position > 0x7FFFFFFFu) return -VFS_EINVAL;
    return (int32_t)position;
}

int32_t vfs_unlink(uint32_t pid, const char *path) {
    uint32_t irq_flags;
    int32_t removed;
    if (!pid_valid(pid) || !path) return -VFS_EINVAL;
    if (!root_fs) return -VFS_EIO;
    irq_flags = cpu_irq_save();
    removed = root_fs->unlink(path);
    cpu_irq_restore(irq_flags);
    return removed;
}

int32_t vfs_rename(uint32_t pid, const char *oldpath, const char *newpath) {
    uint32_t irq_flags;
    int32_t renamed;
    if (!pid_valid(pid) || !oldpath || !newpath) return -VFS_EINVAL;
    if (!root_fs) return -VFS_EIO;
    irq_flags = cpu_irq_save();
    renamed = root_fs->rename(oldpath, newpath);
    cpu_irq_restore(irq_flags);
    return renamed;
}

int32_t vfs_stat(uint32_t pid, const char *path, vfs_stat_t *out) {
    uint32_t irq_flags;
    int32_t stated;
    if (!pid_valid(pid) || !path || !out) return -VFS_EINVAL;
    if (!root_fs) return -VFS_EIO;
    irq_flags = cpu_irq_save();
    stated = root_fs->stat(path, out);
    cpu_irq_restore(irq_flags);
    return stated;
}

int32_t vfs_fstat(uint32_t pid, uint32_t fd, vfs_stat_t *out) {
    vfs_fd_t *descriptor;
    uint32_t irq_flags;
    int32_t stated;
    if (!pid_valid(pid) || fd < VFS_FIRST_FILE_FD || fd >= VFS_FD_COUNT)
        return -VFS_EBADF;
    descriptor = &descriptors[pid - 1u][fd];
    if (!descriptor->used) return -VFS_EBADF;
    if (!out) return -VFS_EINVAL;
    irq_flags = cpu_irq_save();
    stated = descriptor->ops->fstat(descriptor->backend, out);
    cpu_irq_restore(irq_flags);
    return stated;
}

void vfs_close_all(uint32_t pid) {
    if (!pid_valid(pid)) return;
    for (uint32_t fd = VFS_FIRST_FILE_FD; fd < VFS_FD_COUNT; fd++) {
        vfs_fd_t *descriptor = &descriptors[pid - 1u][fd];
        if (!descriptor->used) continue;
        descriptor->ops->close(descriptor->backend);
        memset(descriptor, 0, sizeof(*descriptor));
    }
}
