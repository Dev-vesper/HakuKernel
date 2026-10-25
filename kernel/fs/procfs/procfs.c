/* Haku process filesystem (/proc).
 *
 * A minimal read-only view of kernel state as files, which is what a shell
 * and the usual userland tools reach for first: `version`, `uptime`,
 * `meminfo`, and `loadavg`-style counters. The contents are generated when
 * a file is read, so they always describe the moment of the read rather
 * than a snapshot taken at mount time.
 *
 * Files are small and fixed in number; nothing can be written or created.
 */
#include "procfs.h"
#include "heap.h"
#include "klog.h"
#include "pit.h"
#include "ramfs.h"
#include "vfs.h"

#define PROCFS_MAX_CONTENT 512u

typedef struct {
    const char *name;
    /* Fill buffer with at most size bytes and return the length written. */
    uint32_t (*fill)(char *buffer, uint32_t size);
} procfs_entry_t;

/* A procfs file is generated once, when it is opened, and then read
 * through an offset like any other file. */
typedef struct {
    char content[PROCFS_MAX_CONTENT];
    uint32_t length;
    uint32_t offset;
} procfs_handle_t;

/* Append a decimal number, returning the new offset. */
static uint32_t put_dec(char *buffer, uint32_t size, uint32_t offset,
                        uint32_t value) {
    char digits[11];
    uint32_t count = 0;
    if (offset >= size) return offset;
    if (!value) digits[count++] = '0';
    while (value && count < sizeof(digits)) {
        digits[count++] = (char)('0' + (value % 10u));
        value /= 10u;
    }
    while (count && offset < size) buffer[offset++] = digits[--count];
    return offset;
}

static uint32_t put_text(char *buffer, uint32_t size, uint32_t offset,
                         const char *text) {
    while (*text && offset < size) buffer[offset++] = *text++;
    return offset;
}

static uint32_t fill_version(char *buffer, uint32_t size) {
    uint32_t offset = 0;
    offset = put_text(buffer, size, offset,
                      "HakuKernel 0.1 (i386) #1 SMP-free\n");
    return offset;
}

static uint32_t fill_uptime(char *buffer, uint32_t size) {
    uint32_t hz = pit_hz();
    uint32_t ticks = pit_ticks();
    uint32_t seconds = hz ? ticks / hz : 0;
    uint32_t hundredths = hz ? ((ticks % hz) * 100u) / hz : 0;
    uint32_t offset = 0;
    offset = put_dec(buffer, size, offset, seconds);
    offset = put_text(buffer, size, offset, ".");
    if (hundredths < 10u) offset = put_text(buffer, size, offset, "0");
    offset = put_dec(buffer, size, offset, hundredths);
    offset = put_text(buffer, size, offset, " ");
    offset = put_dec(buffer, size, offset, seconds);
    offset = put_text(buffer, size, offset, ".");
    if (hundredths < 10u) offset = put_text(buffer, size, offset, "0");
    offset = put_dec(buffer, size, offset, hundredths);
    offset = put_text(buffer, size, offset, "\n");
    return offset;
}

static uint32_t fill_meminfo(char *buffer, uint32_t size) {
    uint32_t nodes = 0;
    uint32_t bytes = 0;
    uint32_t offset = 0;
    ramfs_node_usage(&nodes, &bytes);
    offset = put_text(buffer, size, offset, "MemTotal: ");
    offset = put_dec(buffer, size, offset,
                     (HEAP_END - HEAP_START) / 1024u);
    offset = put_text(buffer, size, offset, " kB\nMemFree: ");
    offset = put_dec(buffer, size, offset, heap_free_bytes() / 1024u);
    offset = put_text(buffer, size, offset, " kB\nRamfsBytes: ");
    offset = put_dec(buffer, size, offset, bytes);
    offset = put_text(buffer, size, offset, "\nRamfsNodes: ");
    offset = put_dec(buffer, size, offset, nodes);
    offset = put_text(buffer, size, offset, "\n");
    return offset;
}

static const procfs_entry_t procfs_entries[] = {
    { "version", fill_version },
    { "uptime", fill_uptime },
    { "meminfo", fill_meminfo },
};

#define PROCFS_ENTRY_COUNT (sizeof(procfs_entries) / sizeof(procfs_entries[0]))

static int name_equal(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static const procfs_entry_t *entry_lookup(const char *name) {
    for (uint32_t i = 0; i < PROCFS_ENTRY_COUNT; i++)
        if (name_equal(procfs_entries[i].name, name)) return &procfs_entries[i];
    return 0;
}

static int32_t procfs_mount(void) {
    klog_prefix();
    klog_write("procfs: PROCFS-READY: version uptime meminfo\n");
    return 1;
}

static int32_t procfs_open(const char *path, uint32_t flags,
                           void **handle_out) {
    const procfs_entry_t *entry;
    procfs_handle_t *handle;
    if (flags & (VFS_O_WRONLY | VFS_O_RDWR | VFS_O_CREAT | VFS_O_TRUNC))
        return -VFS_EACCES;
    if (!path || !path[0]) return -VFS_ENOENT;
    if (path[0] == '.' && path[1] == 0) return -VFS_EISDIR;
    entry = entry_lookup(path);
    if (!entry) return -VFS_ENOENT;
    handle = (procfs_handle_t *)kmalloc(sizeof(*handle));
    if (!handle) return -VFS_ENOMEM;
    handle->length = entry->fill(handle->content, sizeof(handle->content));
    handle->offset = 0;
    *handle_out = handle;
    return 1;
}

static int32_t procfs_close(void *handle_ptr) {
    if (!handle_ptr) return -VFS_EINVAL;
    kfree(handle_ptr);
    return 1;
}

static int32_t procfs_read(void *handle_ptr, void *buffer, uint32_t count,
                           uint32_t *out_count) {
    procfs_handle_t *handle = (procfs_handle_t *)handle_ptr;
    uint32_t available;
    if (!handle) return -VFS_EINVAL;
    *out_count = 0;
    if (handle->offset >= handle->length) return 1;
    available = handle->length - handle->offset;
    if (count > available) count = available;
    if (count) memcpy(buffer, handle->content + handle->offset, count);
    handle->offset += count;
    *out_count = count;
    return 1;
}

static int32_t procfs_write(void *handle_ptr, const void *buffer,
                            uint32_t count, uint32_t *out_count) {
    (void)handle_ptr;
    (void)buffer;
    (void)count;
    (void)out_count;
    return -VFS_EACCES;
}

static int32_t procfs_seek(void *handle_ptr, int32_t offset, uint32_t whence,
                           uint32_t *out_pos) {
    procfs_handle_t *handle = (procfs_handle_t *)handle_ptr;
    int32_t base;
    int32_t position;
    if (!handle) return -VFS_EINVAL;
    if (whence == 0u)
        base = 0;
    else if (whence == 1u)
        base = (int32_t)handle->offset;
    else if (whence == 2u)
        base = (int32_t)handle->length;
    else
        return -VFS_EINVAL;
    position = base + offset;
    if (position < 0) return -VFS_EINVAL;
    handle->offset = (uint32_t)position;
    *out_pos = handle->offset;
    return 1;
}

static int32_t procfs_no_unlink(const char *path) {
    (void)path;
    return -VFS_EPERM;
}

static int32_t procfs_no_rename(const char *oldpath, const char *newpath) {
    (void)oldpath;
    (void)newpath;
    return -VFS_EPERM;
}

static int32_t procfs_truncate(void *handle_ptr, uint32_t size) {
    (void)handle_ptr;
    (void)size;
    return -VFS_EACCES;
}

static int32_t procfs_stat(const char *path, vfs_stat_t *out) {
    const procfs_entry_t *entry;
    char content[PROCFS_MAX_CONTENT];
    if (!path || (path[0] == '.' && path[1] == 0)) {
        memset(out, 0, sizeof(*out));
        out->st_dev = 3;
        out->st_ino = 1;
        out->st_mode = HAKU_S_IFDIR | 0555u;
        out->st_nlink = 2;
        out->st_blksize = 512;
        return 1;
    }
    entry = entry_lookup(path);
    if (!entry) return -VFS_ENOENT;
    memset(out, 0, sizeof(*out));
    out->st_dev = 3;
    out->st_ino = 2u + (uint32_t)(entry - procfs_entries);
    out->st_mode = HAKU_S_IFREG | 0444u;
    out->st_nlink = 1;
    out->st_size = entry->fill(content, sizeof(content));
    out->st_blksize = 512;
    out->st_blocks = ((uint32_t)out->st_size + 511u) / 512u;
    return 1;
}

static int32_t procfs_fstat(void *handle_ptr, vfs_stat_t *out) {
    procfs_handle_t *handle = (procfs_handle_t *)handle_ptr;
    if (!handle) return -VFS_EINVAL;
    memset(out, 0, sizeof(*out));
    out->st_dev = 3;
    out->st_ino = 0;
    out->st_mode = HAKU_S_IFREG | 0444u;
    out->st_nlink = 1;
    out->st_size = handle->length;
    out->st_blksize = 512;
    out->st_blocks = ((uint32_t)handle->length + 511u) / 512u;
    return 1;
}

static int32_t procfs_opendir(const char *path, void **handle_out) {
    procfs_handle_t *handle;
    if (path && path[0] && !(path[0] == '.' && path[1] == 0))
        return -VFS_ENOTDIR;
    handle = (procfs_handle_t *)kmalloc(sizeof(*handle));
    if (!handle) return -VFS_ENOMEM;
    memset(handle, 0, sizeof(*handle));
    *handle_out = handle;
    return 1;
}

static int32_t procfs_readdir(void *handle_ptr, uint32_t index,
                              vfs_dirent_t *out, uint32_t *next_index) {
    (void)handle_ptr;
    if (index >= PROCFS_ENTRY_COUNT) return 0;
    memset(out, 0, sizeof(*out));
    out->ino = 2u + index;
    out->type = VFS_DT_REG;
    {
        const char *name = procfs_entries[index].name;
        uint32_t length = 0;
        while (name[length] && length < VFS_DIRENT_NAME_MAX - 1u) length++;
        memcpy(out->name, name, length);
        out->name[length] = 0;
    }
    *next_index = index + 1u;
    return 1;
}

static const vfs_fs_ops_t procfs_ops = {
    "procfs",
    procfs_mount,
    procfs_open,
    procfs_close,
    procfs_read,
    procfs_write,
    procfs_seek,
    procfs_no_unlink,
    procfs_no_rename,
    procfs_no_unlink,
    procfs_no_unlink,
    procfs_stat,
    procfs_fstat,
    procfs_opendir,
    procfs_readdir,
    procfs_close,
    procfs_truncate,
    0,
    0,
};

int procfs_init(void) {
    return vfs_register_fs(&procfs_ops);
}
