/* Haku initramfs unpacker (cpio "newc" archive).
 *
 * The boot archive is a plain cpio archive with the SVR4/"newc" header, the
 * format Linux and every initramfs generator emit. Unpacking it into the
 * mounted RAM filesystem is what makes the userland image independent of
 * the kernel build: the archive is data, so a different userland is a
 * different file, not a different kernel.
 *
 * Entries are created through the VFS rather than by reaching into the
 * ramfs internals, so the archive is unpacked by exactly the code path a
 * user process would take (open/write/close/mkdir/symlink) and every
 * permission, name-length, and space check applies as usual.
 */
#include "initramfs.h"
#include "klog.h"
#include "vfs.h"

#define CPIO_HEADER_SIZE 110u
#define CPIO_MAX_ENTRIES 128u

typedef struct {
    uint32_t mode;
    uint32_t filesize;
    uint32_t namesize;
} cpio_header_t;

static uint32_t hex_value(const uint8_t *text, uint32_t length) {
    uint32_t value = 0;
    for (uint32_t i = 0; i < length; i++) {
        uint8_t c = text[i];
        uint32_t digit;
        if (c >= '0' && c <= '9')
            digit = (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f')
            digit = (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            digit = (uint32_t)(c - 'A' + 10);
        else
            return 0xFFFFFFFFu;
        value = (value << 4) | digit;
    }
    return value;
}

static uint32_t align4(uint32_t value) {
    return (value + 3u) & ~3u;
}

static int header_parse(const uint8_t *raw, cpio_header_t *header) {
    if (raw[0] != '0' || raw[1] != '7' || raw[2] != '0' ||
        raw[3] != '7' || raw[4] != '0')
        return 0;
    if (raw[5] != '1' && raw[5] != '2') /* newc, or newc with CRC */
        return 0;
    /* Field order: ino, mode, uid, gid, nlink, mtime, filesize, devmajor,
     * devminor, rdevmajor, rdevminor, namesize, check. */
    header->mode = hex_value(raw + 14, 8);
    header->filesize = hex_value(raw + 54, 8);
    header->namesize = hex_value(raw + 94, 8);
    if (header->mode == 0xFFFFFFFFu || header->filesize == 0xFFFFFFFFu ||
        header->namesize == 0xFFFFFFFFu || header->namesize == 0)
        return 0;
    return 1;
}

/* Copy an archive name into a canonical path: no leading "./" or "/", and
 * no trailing slash. Returns 0 when the name is unusable. */
static int normalize_name(const uint8_t *name, uint32_t length,
                          char path[VFS_PATH_MAX]) {
    uint32_t used = 0;
    uint32_t i = 0;
    while (i < length && name[i] == '/') i++;
    while (i < length && (name[i] == '.' && name[i + 1u] == '/')) i += 2u;
    for (; i < length && name[i]; i++) {
        if (used + 1u >= VFS_PATH_MAX) return 0;
        path[used++] = (char)name[i];
    }
    while (used && path[used - 1u] == '/') used--;
    path[used] = 0;
    return used > 0;
}

/* mkdir -p for the components of a path, ignoring "already exists". */
static void make_parents(char *path) {
    for (uint32_t i = 1; path[i]; i++) {
        if (path[i] != '/') continue;
        path[i] = 0;
        if (vfs_mkdir(1, path) != 1) {
            /* EEXIST is the normal case; anything else will surface when
             * the entry itself is created. */
        }
        path[i] = '/';
    }
}

static int32_t write_file(const char *path, const uint8_t *data,
                          uint32_t size) {
    int32_t fd;
    uint32_t written = 0;
    fd = vfs_open(1, path, VFS_O_CREAT | VFS_O_WRONLY | VFS_O_TRUNC);
    if (fd < 0) return fd;
    while (written < size) {
        int32_t result = vfs_write(1, (uint32_t)fd, data + written,
                                   size - written);
        if (result < 0) {
            vfs_close(1, (uint32_t)fd);
            return result;
        }
        if (!result) break;
        written += (uint32_t)result;
    }
    vfs_close(1, (uint32_t)fd);
    return written == size ? 1 : -VFS_EIO;
}

int32_t initramfs_unpack(const uint8_t *archive, uint32_t size) {
    uint32_t offset = 0;
    uint32_t entries = 0;
    uint32_t files = 0;
    uint32_t bytes = 0;

    if (!archive || size < CPIO_HEADER_SIZE) return -VFS_EINVAL;
    while (offset + CPIO_HEADER_SIZE <= size) {
        cpio_header_t header;
        char path[VFS_PATH_MAX];
        const uint8_t *name;
        const uint8_t *data;
        uint32_t data_offset;

        if (!header_parse(archive + offset, &header)) return -VFS_EINVAL;
        if (entries++ >= CPIO_MAX_ENTRIES) return -VFS_ENOSPC;
        name = archive + offset + CPIO_HEADER_SIZE;
        if (offset + CPIO_HEADER_SIZE + header.namesize > size)
            return -VFS_EINVAL;
        /* The trailer is a header with the name TRAILER!!! and no data. */
        if (header.namesize >= 9u && name[0] == 'T' && name[1] == 'R' &&
            name[2] == 'A' && name[3] == 'I' && name[4] == 'L')
            break;

        /* The padding covers the header and the name together, not the
         * name alone: 110 is not a multiple of 4. */
        data_offset = offset + align4(CPIO_HEADER_SIZE + header.namesize);
        if (data_offset > size || header.filesize > size - data_offset)
            return -VFS_EINVAL;
        data = archive + data_offset;

        if (!normalize_name(name, header.namesize, path)) {
            /* "." and "./" describe the archive root itself. */
            offset = data_offset + align4(header.filesize);
            continue;
        }

        if ((header.mode & HAKU_S_IFMT) == HAKU_S_IFDIR) {
            make_parents(path);
            if (vfs_mkdir(1, path) != 1) {
                vfs_stat_t status;
                if (vfs_stat(1, path, &status) != 1 ||
                    (status.st_mode & HAKU_S_IFMT) != HAKU_S_IFDIR)
                    return -VFS_EIO;
            }
        } else if ((header.mode & HAKU_S_IFMT) == HAKU_S_IFLNK) {
            char target[VFS_PATH_MAX];
            if (header.filesize >= sizeof(target)) return -VFS_ENAMETOOLONG;
            memcpy(target, data, header.filesize);
            target[header.filesize] = 0;
            make_parents(path);
            if (vfs_symlink(1, target, path) != 1) return -VFS_EIO;
        } else if ((header.mode & HAKU_S_IFMT) == HAKU_S_IFREG) {
            make_parents(path);
            if (write_file(path, data, header.filesize) != 1) return -VFS_EIO;
            files++;
            bytes += header.filesize;
        }
        /* Character devices come from /dev, not from the archive. */

        offset = data_offset + align4(header.filesize);
    }

    if (klog_begin(KLOG_INFO)) {
        klog_write("initramfs: INITRAMFS-UNPACK-OK entries=");
        klog_dec(entries);
        klog_write(" files=");
        klog_dec(files);
        klog_write(" bytes=");
        klog_dec(bytes);
        klog_write("\n");
    }
    return (int32_t)files;
}
