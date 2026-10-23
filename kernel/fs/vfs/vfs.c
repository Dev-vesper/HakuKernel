#include "vfs.h"
#include "idt.h"
#include "scheduler.h"
#include "sync.h"

#define VFS_MAX_PROCESSES SCHEDULER_MAX_PROCESSES
#define VFS_FD_COUNT 8u
#define VFS_FIRST_FILE_FD 3u
#define VFS_MAX_FS 4u
#define VFS_MAX_PIPES 4u
#define VFS_PIPE_CAPACITY 512u
#define VFS_SUPPORTED_FLAGS \
    (VFS_O_ACCMODE | VFS_O_CREAT | VFS_O_EXCL | VFS_O_TRUNC | VFS_O_APPEND | \
     VFS_O_DIRECTORY)

/* An open file description is shared by every descriptor that dup/dup2
 * points at it (Linux semantics): offset, access mode and the backend handle
 * live here, not in the descriptor. refs counts the descriptors still
 * referencing it; the backend handle is released only when the last one
 * closes, so a dup'd descriptor survives its sibling's close and
 * open-on-a-removed-file keeps working until the final close. A description
 * is a regular file (ops/backend), a directory (ops/backend + cursor), a
 * character device such as the console behind the standard descriptors
 * (device), or one end of a pipe. */
typedef enum {
    VFS_OFD_FILE,
    VFS_OFD_DIR,
    VFS_OFD_DEVICE,
    VFS_OFD_PIPE
} vfs_ofd_kind_t;

typedef struct {
    vfs_ofd_kind_t kind;
    const vfs_fs_ops_t *ops;        /* file backend, when kind == FILE/DIR */
    const vfs_device_ops_t *device; /* char device, when kind == DEVICE */
    void *backend;      /* backend-allocated per-open handle */
    uint32_t accmode;   /* VFS_O_RDONLY/WRONLY/RDWR, fixed at open */
    uint32_t status;    /* O_APPEND/O_NONBLOCK, changeable by fcntl */
    uint32_t dir_index; /* enumeration cursor, when kind == DIR */
    char dir_path[VFS_PATH_MAX]; /* canonical path, when kind == DIR */
    uint32_t pipe;      /* pipe index, when kind == PIPE */
    uint32_t refs;
    int used;
} vfs_ofd_t;

typedef struct {
    uint32_t ofd;       /* index into ofd_table when used */
    int used;
} vfs_fd_t;

/* A pipe is a byte ring with one wait queue per direction. Its state is
 * tiny and memory-only, so it is protected by short IRQ-off sections
 * instead of the task-context VFS lock: a blocked reader must be able to
 * park while a writer on another task makes progress. */
typedef struct {
    uint8_t data[VFS_PIPE_CAPACITY];
    uint32_t head;      /* bytes written since the pipe was created */
    uint32_t tail;      /* bytes read since the pipe was created */
    uint32_t readers;
    uint32_t writers;
    wait_queue_t readable; /* readers park here until data arrives */
    wait_queue_t writable; /* writers park here until space frees */
    int used;
} vfs_pipe_t;

static vfs_fd_t descriptors[VFS_MAX_PROCESSES][VFS_FD_COUNT];
static vfs_ofd_t ofd_table[VFS_MAX_PROCESSES * VFS_FD_COUNT];
static const vfs_fs_ops_t *fs_table[VFS_MAX_FS];
static uint32_t fs_count;
static const vfs_fs_ops_t *root_fs;
static int root_read_only;
static const vfs_device_ops_t *stdio_device;
/* Serializes descriptor/OFD-table changes and file/disk backend calls.
 * Task-context spinlock: the holder may be preempted, so interrupts stay
 * enabled during I/O and a waiter busy-waits for the holder to run again. */
static spinlock_t vfs_lock = SPINLOCK_INIT;
static vfs_pipe_t pipes[VFS_MAX_PIPES];

/* Claim a pipe slot. Called with the VFS lock held and before the pipe is
 * reachable through any descriptor. */
static int pipe_alloc(void) {
    for (uint32_t i = 0; i < VFS_MAX_PIPES; i++) {
        if (!pipes[i].used) {
            memset(&pipes[i], 0, sizeof(pipes[i]));
            pipes[i].used = 1;
            return (int)i;
        }
    }
    return -1;
}

/* Release one end of a pipe. Called with the VFS lock held and interrupts
 * disabled (the pipe state is IRQ-protected). */
static void pipe_drop_end(uint32_t index, int write_end) {
    vfs_pipe_t *pipe;
    if (index >= VFS_MAX_PIPES || !pipes[index].used) return;
    pipe = &pipes[index];
    if (write_end) {
        if (pipe->writers) pipe->writers--;
    } else {
        if (pipe->readers) pipe->readers--;
    }
    /* Both directions may now be able to proceed: a reader hitting EOF, a
     * writer hitting EPIPE, or simply more room. */
    wait_queue_wake_all(&pipe->readable);
    wait_queue_wake_all(&pipe->writable);
    if (!pipe->readers && !pipe->writers) pipe->used = 0;
}

/* Blocking pipe read. The IRQ-off section covers the check and the park,
 * so a write can never slip in between them; the park releases the CPU
 * until the writer's wake (or the timer, which simply re-selects us). */
static int32_t pipe_read(vfs_pipe_t *pipe, uint32_t status, void *buffer,
                         uint32_t count) {
    uint8_t *bytes = (uint8_t *)buffer;
    for (;;) {
        irqflags_t flags = irq_lock();
        uint32_t available = pipe->head - pipe->tail;
        if (available) {
            uint32_t take = count < available ? count : available;
            for (uint32_t i = 0; i < take; i++)
                bytes[i] = pipe->data[(pipe->tail + i) % VFS_PIPE_CAPACITY];
            pipe->tail += take;
            wait_queue_cancel(&pipe->readable);
            wait_queue_wake_all(&pipe->writable);
            irq_unlock(flags);
            return (int32_t)take;
        }
        if (!pipe->writers) {
            wait_queue_cancel(&pipe->readable);
            irq_unlock(flags);
            return 0; /* every write end closed: end of file */
        }
        if (status & HAKU_O_NONBLOCK) {
            wait_queue_cancel(&pipe->readable);
            irq_unlock(flags);
            return -VFS_EAGAIN;
        }
        wait_queue_sleep(&pipe->readable);
        __asm {
            sti
            hlt
        }
    }
}

/* Blocking pipe write. Writes are all-or-nothing: a count larger than the
 * free space waits until it fits (Haku pipes are only used for short
 * command pipelines). */
static int32_t pipe_write(vfs_pipe_t *pipe, uint32_t status,
                          const void *buffer, uint32_t count) {
    const uint8_t *bytes = (const uint8_t *)buffer;
    for (;;) {
        irqflags_t flags = irq_lock();
        uint32_t space = VFS_PIPE_CAPACITY - (pipe->head - pipe->tail);
        if (!pipe->readers) {
            irq_unlock(flags);
            /* No reader will ever drain it: Linux raises SIGPIPE and
             * reports EPIPE, and so does Haku. */
            scheduler_kill_pid(scheduler_current_pid(), HAKU_SIGPIPE);
            return -VFS_EPIPE;
        }
        if (count <= space) {
            for (uint32_t i = 0; i < count; i++)
                pipe->data[(pipe->head + i) % VFS_PIPE_CAPACITY] = bytes[i];
            pipe->head += count;
            wait_queue_cancel(&pipe->writable);
            wait_queue_wake_all(&pipe->readable);
            irq_unlock(flags);
            return (int32_t)count;
        }
        if (count > VFS_PIPE_CAPACITY) {
            wait_queue_cancel(&pipe->writable);
            irq_unlock(flags);
            return -VFS_EINVAL;
        }
        if (status & HAKU_O_NONBLOCK) {
            wait_queue_cancel(&pipe->writable);
            irq_unlock(flags);
            return -VFS_EAGAIN;
        }
        wait_queue_sleep(&pipe->writable);
        __asm {
            sti
            hlt
        }
    }
}

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

/* Allocate a free open file description, or -1 when the table is full. */
static int ofd_alloc(void) {
    for (uint32_t i = 0; i < VFS_MAX_PROCESSES * VFS_FD_COUNT; i++)
        if (!ofd_table[i].used) return (int)i;
    return -1;
}

/* Resolve a descriptor to its open file description, or 0 if the slot is
 * empty or out of range. 0-2 are the standard descriptors (console). */
static vfs_ofd_t *fd_ofd(uint32_t pid, uint32_t fd) {
    if (!pid_valid(pid) || fd >= VFS_FD_COUNT) return 0;
    if (!descriptors[pid - 1u][fd].used) return 0;
    return &ofd_table[descriptors[pid - 1u][fd].ofd];
}

/* Drop one descriptor's reference: the backend close runs (IRQ-safely) and
 * the description is torn down only when the count hits zero. Returns the
 * backend's 1 / -VFS_E* result (1 when a sibling still holds it open). */
static int32_t ofd_release(vfs_ofd_t *ofd) {
    int32_t closed = 1;
    if (--ofd->refs == 0) {
        if (ofd->kind == VFS_OFD_DIR) {
            closed = ofd->ops->closedir(ofd->backend);
        } else if (ofd->kind == VFS_OFD_FILE) {
            /* ofd_release may be called with or without the lock held; to keep
             * the backend calls serialized, callers must take vfs_lock before. */
            closed = ofd->ops->close(ofd->backend);
        } else if (ofd->kind == VFS_OFD_PIPE) {
            irqflags_t flags = irq_lock();
            pipe_drop_end(ofd->pipe, ofd->accmode != VFS_O_RDONLY);
            irq_unlock(flags);
        }
        memset(ofd, 0, sizeof(*ofd));
    }
    return closed;
}

/* Allocate a free open file description, or 0 when the table is full. */
static vfs_ofd_t *ofd_create(vfs_ofd_kind_t kind, uint32_t accmode) {
    int index = ofd_alloc();
    vfs_ofd_t *ofd;
    if (index < 0) return 0;
    ofd = &ofd_table[index];
    memset(ofd, 0, sizeof(*ofd));
    ofd->kind = kind;
    ofd->accmode = accmode;
    ofd->refs = 1;
    ofd->used = 1;
    return ofd;
}

static uint32_t ofd_slot(const vfs_ofd_t *ofd) {
    return (uint32_t)(ofd - ofd_table);
}

int vfs_register_fs(const vfs_fs_ops_t *ops) {
    if (!ops || !ops->name || !ops->mount || !ops->open || !ops->close ||
        !ops->read || !ops->write || !ops->seek || !ops->unlink ||
        !ops->rename || !ops->mkdir || !ops->rmdir || !ops->stat ||
        !ops->fstat || !ops->opendir || !ops->readdir || !ops->closedir ||
        !ops->truncate)
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
    return vfs_mount_root_flags(name, 0);
}

int32_t vfs_mount_root_flags(const char *name, uint32_t flags) {
    const vfs_fs_ops_t *ops = 0;
    int32_t mounted;
    if (!name || (flags & ~VFS_MOUNT_RDONLY)) return -VFS_EINVAL;
    for (uint32_t i = 0; i < fs_count; i++) {
        if (name_equal(fs_table[i]->name, name)) {
            ops = fs_table[i];
            break;
        }
    }
    if (!ops) return -VFS_EINVAL;
    spin_lock(&vfs_lock);
    mounted = ops->mount();
    if (mounted != 1) {
        spin_unlock(&vfs_lock);
        return mounted;
    }
    root_fs = ops;
    root_read_only = (flags & VFS_MOUNT_RDONLY) ? 1 : 0;
    spin_unlock(&vfs_lock);
    return 1;
}

void vfs_init(void) {
    memset(descriptors, 0, sizeof(descriptors));
    memset(ofd_table, 0, sizeof(ofd_table));
    memset(pipes, 0, sizeof(pipes));
    fs_count = 0;
    root_fs = 0;
    root_read_only = 0;
    stdio_device = 0;
    spin_init(&vfs_lock);
}

int vfs_register_device(const vfs_device_ops_t *ops) {
    if (!ops || !ops->name || !ops->read || !ops->write) return 0;
    if (stdio_device) return 0;
    stdio_device = ops;
    return 1;
}

/* Point one descriptor slot at a fresh description of the console. */
static int stdio_bind(uint32_t pid, uint32_t fd, uint32_t accmode) {
    vfs_ofd_t *ofd = ofd_create(VFS_OFD_DEVICE, accmode);
    if (!ofd) return 0;
    ofd->device = stdio_device;
    descriptors[pid - 1u][fd].ofd = ofd_slot(ofd);
    descriptors[pid - 1u][fd].used = 1;
    return 1;
}

int vfs_open_stdio(uint32_t pid) {
    int result = 1;
    if (!pid_valid(pid) || !stdio_device) return 0;
    spin_lock(&vfs_lock);
    if (!descriptors[pid - 1u][0].used && !stdio_bind(pid, 0, VFS_O_RDONLY))
        result = 0;
    if (result && !descriptors[pid - 1u][1].used &&
        !stdio_bind(pid, 1, VFS_O_WRONLY))
        result = 0;
    if (result && !descriptors[pid - 1u][2].used &&
        !stdio_bind(pid, 2, VFS_O_WRONLY))
        result = 0;
    spin_unlock(&vfs_lock);
    return result;
}

int32_t vfs_inherit(uint32_t parent_pid, uint32_t child_pid) {
    if (!pid_valid(parent_pid) || !pid_valid(child_pid))
        return -VFS_EINVAL;
    if (parent_pid == child_pid) return -VFS_EINVAL;
    spin_lock(&vfs_lock);
    /* A reused pid must start clean; releasing stale descriptors is a
     * no-op when the slot was already released on its previous exit. */
    for (uint32_t fd = 0; fd < VFS_FD_COUNT; fd++) {
        vfs_fd_t *slot = &descriptors[child_pid - 1u][fd];
        if (slot->used) {
            slot->used = 0;
            ofd_release(&ofd_table[slot->ofd]);
        }
    }
    for (uint32_t fd = 0; fd < VFS_FD_COUNT; fd++) {
        vfs_fd_t *parent = &descriptors[parent_pid - 1u][fd];
        if (!parent->used) continue;
        ofd_table[parent->ofd].refs++;
        descriptors[child_pid - 1u][fd] = *parent;
        descriptors[child_pid - 1u][fd].used = 1;
    }
    spin_unlock(&vfs_lock);
    return 1;
}

uint32_t vfs_open_count(uint32_t pid) {
    uint32_t count = 0;
    if (!pid_valid(pid)) return 0;
    spin_lock(&vfs_lock);
    for (uint32_t fd = 0; fd < VFS_FD_COUNT; fd++)
        if (descriptors[pid - 1u][fd].used) count++;
    spin_unlock(&vfs_lock);
    return count;
}

int32_t vfs_open(uint32_t pid, const char *path, uint32_t flags) {
    vfs_fd_t *slot;
    vfs_ofd_t *ofd;
    uint32_t fd;
    int ofd_index;
    void *handle = 0;
    int32_t opened;
    if (!pid_valid(pid) || !path || (flags & ~VFS_SUPPORTED_FLAGS) ||
        (flags & VFS_O_ACCMODE) == VFS_O_ACCMODE ||
        ((flags & VFS_O_EXCL) && !(flags & VFS_O_CREAT)) ||
        ((flags & VFS_O_TRUNC) &&
         (flags & VFS_O_ACCMODE) == VFS_O_RDONLY))
        return -VFS_EINVAL;
    /* A directory opens for enumeration only: creation and truncation do
     * not apply to it. */
    if (flags & VFS_O_DIRECTORY) {
        if (flags & (VFS_O_CREAT | VFS_O_EXCL | VFS_O_TRUNC))
            return -VFS_EINVAL;
        return vfs_opendir(pid, path);
    }
    spin_lock(&vfs_lock);
    if (root_read_only &&
        ((flags & VFS_O_ACCMODE) != VFS_O_RDONLY ||
         (flags & (VFS_O_CREAT | VFS_O_TRUNC)))) {
        spin_unlock(&vfs_lock);
        return -VFS_EROFS;
    }
    if (!root_fs) {
        spin_unlock(&vfs_lock);
        return -VFS_EIO;
    }
    for (fd = VFS_FIRST_FILE_FD; fd < VFS_FD_COUNT; fd++)
        if (!descriptors[pid - 1u][fd].used) break;
    if (fd == VFS_FD_COUNT) {
        spin_unlock(&vfs_lock);
        return -VFS_EMFILE;
    }
    ofd_index = ofd_alloc();
    if (ofd_index < 0) {
        spin_unlock(&vfs_lock);
        return -VFS_EMFILE;
    }

    opened = root_fs->open(path, flags, &handle);
    if (opened != 1) {
        spin_unlock(&vfs_lock);
        return opened;
    }
    ofd = &ofd_table[ofd_index];
    ofd->kind = VFS_OFD_FILE;
    ofd->ops = root_fs;
    ofd->accmode = flags & VFS_O_ACCMODE;
    ofd->status = flags & (VFS_O_APPEND | HAKU_O_NONBLOCK);
    ofd->backend = handle;
    ofd->refs = 1;
    ofd->used = 1;
    slot = &descriptors[pid - 1u][fd];
    slot->ofd = (uint32_t)ofd_index;
    slot->used = 1;
    spin_unlock(&vfs_lock);
    return (int32_t)fd;
}

int32_t vfs_close(uint32_t pid, uint32_t fd) {
    vfs_fd_t *slot;
    vfs_ofd_t *ofd;
    int32_t closed;
    if (!pid_valid(pid) || fd >= VFS_FD_COUNT) return -VFS_EBADF;
    spin_lock(&vfs_lock);
    slot = &descriptors[pid - 1u][fd];
    if (!slot->used) {
        spin_unlock(&vfs_lock);
        return -VFS_EBADF;
    }
    ofd = &ofd_table[slot->ofd];
    /* Linux detach-on-close: the descriptor is gone even when the backend
     * reports an error; the errno still reaches the caller. */
    slot->used = 0;
    closed = ofd_release(ofd);
    spin_unlock(&vfs_lock);
    return closed == 1 ? 0 : closed;
}

int32_t vfs_dup(uint32_t pid, uint32_t fd) {
    vfs_ofd_t *ofd;
    uint32_t newfd;
    spin_lock(&vfs_lock);
    ofd = fd_ofd(pid, fd);
    if (!ofd) {
        spin_unlock(&vfs_lock);
        return -VFS_EBADF;
    }
    for (newfd = 0; newfd < VFS_FD_COUNT; newfd++)
        if (!descriptors[pid - 1u][newfd].used) break;
    if (newfd == VFS_FD_COUNT) {
        spin_unlock(&vfs_lock);
        return -VFS_EMFILE;
    }
    ofd->refs++;
    descriptors[pid - 1u][newfd].ofd = descriptors[pid - 1u][fd].ofd;
    descriptors[pid - 1u][newfd].used = 1;
    spin_unlock(&vfs_lock);
    return (int32_t)newfd;
}

int32_t vfs_dup2(uint32_t pid, uint32_t oldfd, uint32_t newfd) {
    vfs_fd_t *slot;
    vfs_ofd_t *ofd;
    if (!pid_valid(pid) || oldfd >= VFS_FD_COUNT || newfd >= VFS_FD_COUNT)
        return -VFS_EBADF;
    spin_lock(&vfs_lock);
    if (!descriptors[pid - 1u][oldfd].used) {
        spin_unlock(&vfs_lock);
        return -VFS_EBADF;
    }
    if (oldfd == newfd) {
        spin_unlock(&vfs_lock);
        return (int32_t)newfd;
    }
    ofd = fd_ofd(pid, newfd);
    if (ofd) {
        /* Target was open: POSIX closes it first and does not report a
         * failure here, so the dup2 outcome is unaffected by the close. */
        vfs_ofd_t *victim = &ofd_table[descriptors[pid - 1u][newfd].ofd];
        descriptors[pid - 1u][newfd].used = 0;
        ofd_release(victim);
    }
    ofd_table[descriptors[pid - 1u][oldfd].ofd].refs++;
    slot = &descriptors[pid - 1u][newfd];
    slot->ofd = descriptors[pid - 1u][oldfd].ofd;
    slot->used = 1;
    spin_unlock(&vfs_lock);
    return (int32_t)newfd;
}

int32_t vfs_read(uint32_t pid, uint32_t fd, void *buffer, uint32_t count) {
    vfs_ofd_t *ofd;
    const vfs_device_ops_t *device;
    uint32_t copied = 0;
    int32_t read_result;
    if (!buffer && count) return -VFS_EINVAL;
    spin_lock(&vfs_lock);
    ofd = fd_ofd(pid, fd);
    if (!ofd) {
        spin_unlock(&vfs_lock);
        return -VFS_EBADF;
    }
    if (ofd->accmode == VFS_O_WRONLY) {
        spin_unlock(&vfs_lock);
        return -VFS_EBADF;
    }
    if (ofd->kind == VFS_OFD_DIR) {
        spin_unlock(&vfs_lock);
        return -VFS_EISDIR;
    }
    if (ofd->kind == VFS_OFD_PIPE) {
        vfs_pipe_t *pipe = &pipes[ofd->pipe];
        uint32_t status = ofd->status;
        spin_unlock(&vfs_lock);
        return pipe_read(pipe, status, buffer, count);
    }
    if (ofd->kind == VFS_OFD_DEVICE) {
        /* Devices may block (console input), so the VFS lock is dropped
         * before calling them. */
        device = ofd->device;
        spin_unlock(&vfs_lock);
        read_result = device->read(buffer, count, &copied);
    } else {
        read_result = ofd->ops->read(ofd->backend, buffer, count, &copied);
        spin_unlock(&vfs_lock);
    }
    if (copied) return (int32_t)copied;
    if (read_result == 1) return 0;
    return read_result;
}

int32_t vfs_write(uint32_t pid, uint32_t fd, const void *buffer,
                  uint32_t count) {
    vfs_ofd_t *ofd;
    const vfs_device_ops_t *device;
    uint32_t written = 0;
    int32_t write_result;
    if (!buffer && count) return -VFS_EINVAL;
    spin_lock(&vfs_lock);
    ofd = fd_ofd(pid, fd);
    if (!ofd) {
        spin_unlock(&vfs_lock);
        return -VFS_EBADF;
    }
    if (ofd->accmode == VFS_O_RDONLY) {
        spin_unlock(&vfs_lock);
        return -VFS_EBADF;
    }
    if (ofd->kind == VFS_OFD_DIR) {
        spin_unlock(&vfs_lock);
        return -VFS_EISDIR;
    }
    if (ofd->kind == VFS_OFD_PIPE) {
        vfs_pipe_t *pipe = &pipes[ofd->pipe];
        uint32_t status = ofd->status;
        spin_unlock(&vfs_lock);
        return pipe_write(pipe, status, buffer, count);
    }
    if (ofd->kind == VFS_OFD_FILE && root_read_only) {
        spin_unlock(&vfs_lock);
        return -VFS_EROFS;
    }
    if (ofd->kind == VFS_OFD_FILE && (ofd->status & VFS_O_APPEND)) {
        uint32_t position = 0;
        /* fcntl(F_SETFL, O_APPEND) must take effect on a description that
         * was opened without it: reposition to the end before every
         * write. A backend that cannot seek simply writes at its cursor. */
        ofd->ops->seek(ofd->backend, 0, 2u, &position);
    }
    if (ofd->kind == VFS_OFD_DEVICE) {
        device = ofd->device;
        spin_unlock(&vfs_lock);
        write_result = device->write(buffer, count, &written);
    } else {
        write_result = ofd->ops->write(ofd->backend, buffer, count, &written);
        spin_unlock(&vfs_lock);
    }
    if (written) return (int32_t)written;
    if (write_result == 1) return 0;
    return write_result;
}

int32_t vfs_seek(uint32_t pid, uint32_t fd, int32_t offset,
                 uint32_t whence) {
    vfs_ofd_t *ofd;
    uint32_t position = 0;
    int32_t moved;
    spin_lock(&vfs_lock);
    ofd = fd_ofd(pid, fd);
    if (!ofd) {
        spin_unlock(&vfs_lock);
        return -VFS_EBADF;
    }
    if (ofd->kind != VFS_OFD_FILE) {
        spin_unlock(&vfs_lock);
        return -VFS_ESPIPE;
    }
    moved = ofd->ops->seek(ofd->backend, offset, whence, &position);
    spin_unlock(&vfs_lock);
    if (moved != 1) return moved;
    if (position > 0x7FFFFFFFu) return -VFS_EINVAL;
    return (int32_t)position;
}

int32_t vfs_unlink(uint32_t pid, const char *path) {
    int32_t removed;
    if (!pid_valid(pid) || !path) return -VFS_EINVAL;
    spin_lock(&vfs_lock);
    if (!root_fs) {
        spin_unlock(&vfs_lock);
        return -VFS_EIO;
    }
    if (root_read_only) {
        spin_unlock(&vfs_lock);
        return -VFS_EROFS;
    }
    removed = root_fs->unlink(path);
    spin_unlock(&vfs_lock);
    return removed;
}

int32_t vfs_rename(uint32_t pid, const char *oldpath, const char *newpath) {
    int32_t renamed;
    if (!pid_valid(pid) || !oldpath || !newpath) return -VFS_EINVAL;
    spin_lock(&vfs_lock);
    if (!root_fs) {
        spin_unlock(&vfs_lock);
        return -VFS_EIO;
    }
    if (root_read_only) {
        spin_unlock(&vfs_lock);
        return -VFS_EROFS;
    }
    renamed = root_fs->rename(oldpath, newpath);
    spin_unlock(&vfs_lock);
    return renamed;
}

int32_t vfs_mkdir(uint32_t pid, const char *path) {
    int32_t created;
    if (!pid_valid(pid) || !path) return -VFS_EINVAL;
    spin_lock(&vfs_lock);
    if (!root_fs) {
        spin_unlock(&vfs_lock);
        return -VFS_EIO;
    }
    if (root_read_only) {
        spin_unlock(&vfs_lock);
        return -VFS_EROFS;
    }
    created = root_fs->mkdir(path);
    spin_unlock(&vfs_lock);
    return created;
}

int32_t vfs_rmdir(uint32_t pid, const char *path) {
    int32_t removed;
    if (!pid_valid(pid) || !path) return -VFS_EINVAL;
    spin_lock(&vfs_lock);
    if (!root_fs) {
        spin_unlock(&vfs_lock);
        return -VFS_EIO;
    }
    if (root_read_only) {
        spin_unlock(&vfs_lock);
        return -VFS_EROFS;
    }
    removed = root_fs->rmdir(path);
    spin_unlock(&vfs_lock);
    return removed;
}

int32_t vfs_stat(uint32_t pid, const char *path, vfs_stat_t *out) {
    int32_t stated;
    if (!pid_valid(pid) || !path || !out) return -VFS_EINVAL;
    spin_lock(&vfs_lock);
    if (!root_fs) {
        spin_unlock(&vfs_lock);
        return -VFS_EIO;
    }
    stated = root_fs->stat(path, out);
    spin_unlock(&vfs_lock);
    return stated;
}

int32_t vfs_fstat(uint32_t pid, uint32_t fd, vfs_stat_t *out) {
    vfs_ofd_t *ofd;
    int32_t stated;
    if (!out) return -VFS_EINVAL;
    spin_lock(&vfs_lock);
    ofd = fd_ofd(pid, fd);
    if (!ofd) {
        spin_unlock(&vfs_lock);
        return -VFS_EBADF;
    }
    if (ofd->kind != VFS_OFD_FILE) {
        spin_unlock(&vfs_lock);
        return -VFS_EINVAL;
    }
    stated = ofd->ops->fstat(ofd->backend, out);
    spin_unlock(&vfs_lock);
    return stated;
}

void vfs_close_all(uint32_t pid) {
    if (!pid_valid(pid)) return;
    for (uint32_t fd = 0; fd < VFS_FD_COUNT; fd++) {
        vfs_fd_t *slot = &descriptors[pid - 1u][fd];
        if (!slot->used) continue;
        slot->used = 0;
        spin_lock(&vfs_lock);
        ofd_release(&ofd_table[slot->ofd]);
        spin_unlock(&vfs_lock);
    }
}

int32_t vfs_opendir(uint32_t pid, const char *path) {
    vfs_fd_t *slot;
    vfs_ofd_t *ofd;
    uint32_t fd;
    int ofd_index;
    void *handle = 0;
    int32_t opened;
    if (!pid_valid(pid) || !path) return -VFS_EINVAL;
    spin_lock(&vfs_lock);
    for (fd = VFS_FIRST_FILE_FD; fd < VFS_FD_COUNT; fd++)
        if (!descriptors[pid - 1u][fd].used) break;
    if (fd == VFS_FD_COUNT) {
        spin_unlock(&vfs_lock);
        return -VFS_EMFILE;
    }
    ofd_index = ofd_alloc();
    if (ofd_index < 0) {
        spin_unlock(&vfs_lock);
        return -VFS_EMFILE;
    }
    opened = root_fs->opendir(path, &handle);
    if (opened != 1) {
        spin_unlock(&vfs_lock);
        return opened;
    }
    ofd = &ofd_table[ofd_index];
    ofd->kind = VFS_OFD_DIR;
    ofd->ops = root_fs;
    ofd->backend = handle;
    ofd->accmode = VFS_O_RDONLY;
    ofd->status = 0;
    ofd->dir_index = 0;
    {
        uint32_t length = (uint32_t)strlen(path);
        if (length >= sizeof(ofd->dir_path))
            length = sizeof(ofd->dir_path) - 1u;
        memcpy(ofd->dir_path, path, length);
        ofd->dir_path[length] = 0;
    }
    ofd->refs = 1;
    ofd->used = 1;
    slot = &descriptors[pid - 1u][fd];
    slot->ofd = (uint32_t)ofd_index;
    slot->used = 1;
    spin_unlock(&vfs_lock);
    return (int32_t)fd;
}

int32_t vfs_getdents(uint32_t pid, uint32_t fd, void *buffer, uint32_t count) {
    vfs_ofd_t *ofd;
    vfs_dirent_t entry;
    uint32_t next;
    uint32_t used = 0;
    int got;
    if (!buffer || count < VFS_DIRENT_RECORD_MAX) return -VFS_EINVAL;
    spin_lock(&vfs_lock);
    ofd = fd_ofd(pid, fd);
    if (!ofd) {
        spin_unlock(&vfs_lock);
        return -VFS_EBADF;
    }
    if (ofd->kind != VFS_OFD_DIR) {
        spin_unlock(&vfs_lock);
        return -VFS_ENOTDIR;
    }
    for (;;) {
        uint32_t namelen;
        uint32_t reclen;
        if (count - used < 8u) break;
        got = ofd->ops->readdir(ofd->backend, ofd->dir_index, &entry, &next);
        if (got != 1) {
            if (got < 0 && used == 0) {
                spin_unlock(&vfs_lock);
                return got;
            }
            break;
        }
        namelen = 0;
        while (entry.name[namelen] && namelen < VFS_DIRENT_NAME_MAX - 1u)
            namelen++;
        reclen = (19u + namelen + 8u) & ~7u;
        if (reclen > count - used) break; /* did not fit: leave it unconsumed */
        ofd->dir_index = next;
        {
            uint8_t *rec = (uint8_t *)buffer + used;
            uint32_t ino = (uint32_t)entry.ino;
            for (uint32_t i = 0; i < 4; i++) rec[i] = (uint8_t)(ino >> (8 * i));
            for (uint32_t i = 0; i < 4; i++) rec[4 + i] = 0;
            for (uint32_t i = 0; i < 4; i++) rec[8 + i] = (uint8_t)(ofd->dir_index >> (8 * i));
            for (uint32_t i = 0; i < 4; i++) rec[12 + i] = 0;
            rec[16] = (uint8_t)(reclen & 0xFFu);
            rec[17] = (uint8_t)((reclen >> 8) & 0xFFu);
            rec[18] = (uint8_t)entry.type;
            memcpy(rec + 19, entry.name, namelen + 1);
            memset(rec + 19 + namelen + 1, 0, reclen - (19u + namelen + 1u));
        }
        used += reclen;
    }
    spin_unlock(&vfs_lock);
    return (int32_t)used;
}

int32_t vfs_pipe(uint32_t pid, uint32_t *fds) {
    int pipe_index;
    int reader_ofd;
    int writer_ofd;
    uint32_t read_fd;
    uint32_t write_fd;
    vfs_pipe_t *pipe;
    if (!pid_valid(pid) || !fds) return -VFS_EINVAL;
    spin_lock(&vfs_lock);
    for (read_fd = 0; read_fd < VFS_FD_COUNT; read_fd++)
        if (!descriptors[pid - 1u][read_fd].used) break;
    if (read_fd == VFS_FD_COUNT) {
        spin_unlock(&vfs_lock);
        return -VFS_EMFILE;
    }
    for (write_fd = read_fd + 1u; write_fd < VFS_FD_COUNT; write_fd++)
        if (!descriptors[pid - 1u][write_fd].used) break;
    if (write_fd == VFS_FD_COUNT) {
        spin_unlock(&vfs_lock);
        return -VFS_EMFILE;
    }
    pipe_index = pipe_alloc();
    if (pipe_index < 0) {
        spin_unlock(&vfs_lock);
        return -VFS_EMFILE;
    }
    reader_ofd = ofd_alloc();
    if (reader_ofd < 0) {
        pipes[pipe_index].used = 0;
        spin_unlock(&vfs_lock);
        return -VFS_EMFILE;
    }
    /* ofd_alloc only finds a free slot; mark it used before asking for
     * the second one, or both ends would share a single description. */
    memset(&ofd_table[reader_ofd], 0, sizeof(ofd_table[reader_ofd]));
    ofd_table[reader_ofd].kind = VFS_OFD_PIPE;
    ofd_table[reader_ofd].pipe = (uint32_t)pipe_index;
    ofd_table[reader_ofd].accmode = VFS_O_RDONLY;
    ofd_table[reader_ofd].refs = 1;
    ofd_table[reader_ofd].used = 1;
    writer_ofd = ofd_alloc();
    if (writer_ofd < 0) {
        ofd_table[reader_ofd].used = 0;
        pipes[pipe_index].used = 0;
        spin_unlock(&vfs_lock);
        return -VFS_EMFILE;
    }
    /* Two descriptions, not one shared one: closing the write end must
     * drop the writer count even while the reader stays open, otherwise
     * a reader would never see end of file. */
    pipe = &pipes[pipe_index];
    memset(pipe, 0, sizeof(*pipe));
    pipe->used = 1;
    pipe->readers = 1;
    pipe->writers = 1;
    memset(&ofd_table[writer_ofd], 0, sizeof(ofd_table[writer_ofd]));
    ofd_table[writer_ofd].kind = VFS_OFD_PIPE;
    ofd_table[writer_ofd].pipe = (uint32_t)pipe_index;
    ofd_table[writer_ofd].accmode = VFS_O_WRONLY;
    ofd_table[writer_ofd].refs = 1;
    ofd_table[writer_ofd].used = 1;
    descriptors[pid - 1u][read_fd].ofd = (uint32_t)reader_ofd;
    descriptors[pid - 1u][read_fd].used = 1;
    descriptors[pid - 1u][write_fd].ofd = (uint32_t)writer_ofd;
    descriptors[pid - 1u][write_fd].used = 1;
    spin_unlock(&vfs_lock);
    fds[0] = read_fd;
    fds[1] = write_fd;
    return 1;
}

int32_t vfs_dup_min(uint32_t pid, uint32_t fd, uint32_t minfd) {
    vfs_ofd_t *ofd;
    uint32_t newfd;
    if (!pid_valid(pid) || fd >= VFS_FD_COUNT || minfd >= VFS_FD_COUNT)
        return -VFS_EINVAL;
    spin_lock(&vfs_lock);
    ofd = fd_ofd(pid, fd);
    if (!ofd) {
        spin_unlock(&vfs_lock);
        return -VFS_EBADF;
    }
    for (newfd = minfd; newfd < VFS_FD_COUNT; newfd++)
        if (!descriptors[pid - 1u][newfd].used) break;
    if (newfd == VFS_FD_COUNT) {
        spin_unlock(&vfs_lock);
        return -VFS_EMFILE;
    }
    ofd->refs++;
    descriptors[pid - 1u][newfd].ofd = descriptors[pid - 1u][fd].ofd;
    descriptors[pid - 1u][newfd].used = 1;
    spin_unlock(&vfs_lock);
    return (int32_t)newfd;
}

int32_t vfs_get_status_flags(uint32_t pid, uint32_t fd) {
    vfs_ofd_t *ofd;
    uint32_t status = 0;
    if (!pid_valid(pid) || fd >= VFS_FD_COUNT) return -VFS_EBADF;
    spin_lock(&vfs_lock);
    ofd = fd_ofd(pid, fd);
    if (!ofd) {
        spin_unlock(&vfs_lock);
        return -VFS_EBADF;
    }
    status = ofd->accmode | (ofd->status & (VFS_O_APPEND | HAKU_O_NONBLOCK));
    spin_unlock(&vfs_lock);
    return (int32_t)status;
}

int32_t vfs_set_status_flags(uint32_t pid, uint32_t fd, uint32_t flags) {
    vfs_ofd_t *ofd;
    if (!pid_valid(pid) || fd >= VFS_FD_COUNT) return -VFS_EBADF;
    if (flags & ~(VFS_O_APPEND | HAKU_O_NONBLOCK)) {
        /* F_GETLK/F_SETLK/F_SETLKW have no POSIX record-lock support yet. */
        return -VFS_ENOLCK;
    }
    spin_lock(&vfs_lock);
    ofd = fd_ofd(pid, fd);
    if (!ofd) {
        spin_unlock(&vfs_lock);
        return -VFS_EBADF;
    }
    ofd->status = flags;
    spin_unlock(&vfs_lock);
    return 1;
}

int32_t vfs_ftruncate(uint32_t pid, uint32_t fd, uint32_t length) {
    vfs_ofd_t *ofd;
    int32_t moved;
    if (!pid_valid(pid) || fd >= VFS_FD_COUNT) return -VFS_EBADF;
    spin_lock(&vfs_lock);
    ofd = fd_ofd(pid, fd);
    if (!ofd) {
        spin_unlock(&vfs_lock);
        return -VFS_EBADF;
    }
    if (ofd->kind != VFS_OFD_FILE) {
        spin_unlock(&vfs_lock);
        return -VFS_EISDIR;
    }
    if (ofd->accmode == VFS_O_RDONLY) {
        spin_unlock(&vfs_lock);
        return -VFS_EACCES;
    }
    if (!ofd->ops->truncate) {
        spin_unlock(&vfs_lock);
        return -VFS_EOPNOTSUPP;
    }
    moved = ofd->ops->truncate(ofd->backend, length);
    spin_unlock(&vfs_lock);
    return moved;
}

uint32_t vfs_poll_events(uint32_t pid, uint32_t fd, int for_write) {
    vfs_ofd_t *ofd;
    vfs_pipe_t *pipe;
    uint32_t events = 0;
    if (!pid_valid(pid) || fd >= VFS_FD_COUNT) return VFS_POLLNVAL;
    spin_lock(&vfs_lock);
    ofd = fd_ofd(pid, fd);
    if (!ofd) {
        spin_unlock(&vfs_lock);
        return VFS_POLLNVAL;
    }
    if (ofd->kind == VFS_OFD_DEVICE) {
        int ready = 0;
        if (ofd->device->readable) ready = ofd->device->readable();
        spin_unlock(&vfs_lock);
        return for_write ? HAKU_POLLOUT : (ready ? HAKU_POLLIN : 0);
    }
    if (ofd->kind == VFS_OFD_PIPE) {
        irqflags_t irq_flags;
        pipe = &pipes[ofd->pipe];
        irq_flags = irq_lock();
        if (for_write) {
            if (!pipe->readers) events = HAKU_POLLERR;
            else events = (pipe->head - pipe->tail) < VFS_PIPE_CAPACITY ? HAKU_POLLOUT : 0;
        } else {
            if (!pipe->writers) events = HAKU_POLLIN | HAKU_POLLHUP;
            else events = (pipe->head - pipe->tail) > 0 ? HAKU_POLLIN : 0;
        }
        irq_unlock(irq_flags);
        spin_unlock(&vfs_lock);
        return events;
    }
    if (ofd->kind == VFS_OFD_DIR) {
        spin_unlock(&vfs_lock);
        return for_write ? HAKU_POLLOUT : HAKU_POLLIN;
    }
    if (ofd->kind == VFS_OFD_FILE) {
        if (for_write) { spin_unlock(&vfs_lock); return HAKU_POLLOUT; }
        spin_unlock(&vfs_lock);
        return HAKU_POLLIN;
    }
    spin_unlock(&vfs_lock);
    return VFS_POLLNVAL;
}

int vfs_is_console(uint32_t pid, uint32_t fd) {    vfs_ofd_t *ofd;
    int result = 0;
    spin_lock(&vfs_lock);
    ofd = fd_ofd(pid, fd);
    if (ofd && ofd->kind == VFS_OFD_DEVICE) result = 1;
    spin_unlock(&vfs_lock);
    return result;
}

/* The canonical path behind a directory descriptor, so the *at family can
 * resolve a relative name against the descriptor the caller passed. */
int32_t vfs_dir_path(uint32_t pid, uint32_t fd, char out[VFS_PATH_MAX]) {
    vfs_ofd_t *ofd;
    int32_t result;
    if (!out) return -VFS_EINVAL;
    spin_lock(&vfs_lock);
    ofd = fd_ofd(pid, fd);
    if (!ofd) {
        spin_unlock(&vfs_lock);
        return -VFS_EBADF;
    }
    if (ofd->kind != VFS_OFD_DIR) {
        spin_unlock(&vfs_lock);
        return -VFS_ENOTDIR;
    }
    memcpy(out, ofd->dir_path, VFS_PATH_MAX);
    result = 1;
    spin_unlock(&vfs_lock);
    return result;
}
