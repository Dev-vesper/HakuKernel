#include "usermode.h"
#include "console.h"
#include "fat12_vfs.h"
#include "kbd.h"
#include "klog.h"
#include "paging.h"
#include "scheduler.h"
#include "fat12.h"
#include "heap.h"
#include "vfs.h"

#define USER_ELF_MAX_SIZE (256u * 1024u)

static loaded_elf_t programs[2];
#define USER_PROGRAM_COUNT (sizeof(programs) / sizeof(programs[0]))

static int elf_rejection_self_test(const uint8_t *image,
                                   uint32_t image_size) {
    uint8_t invalid[116];
    loaded_elf_t rejected;

    if (image_size < sizeof(invalid)) return 0;
    memcpy(invalid, image, sizeof(invalid));
    invalid[0] = 0;
    if (elf_load_executable(invalid, sizeof(invalid), &rejected)) return 0;

    memcpy(invalid, image, sizeof(invalid));
    invalid[28] = 0xF0;
    invalid[29] = 0xFF;
    invalid[30] = 0xFF;
    invalid[31] = 0xFF;
    if (elf_load_executable(invalid, sizeof(invalid), &rejected)) return 0;

    memcpy(invalid, image, sizeof(invalid));
    invalid[76] = 7; /* first PT_LOAD would be writable and executable */
    if (elf_load_executable(invalid, sizeof(invalid), &rejected)) return 0;

    klog_prefix();
    klog_write("ELF-REJECT-OK: malformed headers and W+X rejected\n");
    return 1;
}

int usermode_init(void) {
    uint32_t loaded = 0;
    uint32_t image_size = 0;
    uint8_t *image;

    /* Assemble the storage stack: VFS tables, then the FAT12 backend as
     * the root filesystem. Everything below (syscalls, self-tests) only
     * speaks the vfs_* interface from here on. */
    vfs_init();
    if (!vfs_register_fs(&fat12_fs_ops)) return 0;
    if (vfs_mount_root("fat12") != 1) return 0;
    image = (uint8_t *)kmalloc(USER_ELF_MAX_SIZE);
    if (!image) {
        klog_prefix();
        klog_write("ELF-DISK-FAIL: kernel heap exhausted\n");
        return 0;
    }
    if (fat12_read_file("HAKU.ELF", image, USER_ELF_MAX_SIZE,
                        &image_size) != 1) {
        kfree(image);
        return 0;
    }
    klog_prefix();
    klog_write("FAT12-READ-OK name=HAKU.ELF size=");
    klog_dec(image_size);
    klog_write("\n");

    if (!elf_rejection_self_test(image, image_size)) {
        klog_prefix();
        klog_write("ELF-REJECT-FAIL: bad image accepted\n");
        kfree(image);
        return 0;
    }
    for (; loaded < USER_PROGRAM_COUNT; loaded++) {
        if (!elf_load_executable(image, image_size, &programs[loaded])) {
            klog_prefix();
            klog_write("ELF-LOAD-FAIL: invalid or unsupported ELF32 image\n");
            while (loaded)
                paging_destroy_user_space(
                    &programs[--loaded].address_space);
            kfree(image);
            return 0;
        }
    }
    kfree(image);
    klog_prefix();
    klog_write("ELF-LOAD-OK: entry=");
    klog_hex(programs[0].entry);
    klog_write(" cr3=");
    klog_hex(programs[0].address_space.directory_phys);
    klog_write("\n");
    klog_prefix();
    klog_write("USERMODE-READY: user stack=");
    klog_hex(programs[0].user_stack);
    klog_write("\n");
    if (!scheduler_init(programs, USER_PROGRAM_COUNT)) {
        klog_prefix();
        klog_write("SCHED-INIT-FAIL: no task slots\n");
        for (uint32_t i = 0; i < USER_PROGRAM_COUNT; i++)
            paging_destroy_user_space(&programs[i].address_space);
        return 0;
    }
    return 1;
}

void usermode_run(void) {
    klog_prefix();
    klog_write("USERMODE-ENTER: iret to CPL 3\n");
    scheduler_start();
}

static uint32_t copy_from_user_error(uint32_t error) {
    return 0u - error;
}

static int copy_from_user(void *destination, uint32_t source,
                          uint32_t length) {
    if (!paging_user_range_valid(source, length, 0)) return 0;
    if (length) memcpy(destination, (const void *)source, length);
    return 1;
}

static int copy_to_user(uint32_t destination, const void *source,
                        uint32_t length) {
    if (!paging_user_range_valid(destination, length, 1)) return 0;
    if (length) memcpy((void *)destination, source, length);
    return 1;
}

static uint32_t sys_write(uint32_t fd, uint32_t user_buffer,
                          uint32_t count) {
    uint8_t chunk[128];
    uint32_t offset = 0;

    if (count > USER_IO_MAX)
        return copy_from_user_error(HAKU_EINVAL);
    if (!paging_user_range_valid(user_buffer, count, 0))
        return copy_from_user_error(HAKU_EFAULT);
    if (fd > 2u) {
        int32_t result;
        if (count == 0)
            return (uint32_t)vfs_write(scheduler_current_pid(), fd, 0, 0);
        while (offset < count) {
            uint32_t amount = count - offset;
            if (amount > sizeof(chunk)) amount = sizeof(chunk);
            if (!copy_from_user(chunk, user_buffer + offset, amount))
                return offset ? offset : copy_from_user_error(HAKU_EFAULT);
            result = vfs_write(scheduler_current_pid(), fd, chunk, amount);
            if (result < 0)
                return offset ? offset : (uint32_t)result;
            offset += (uint32_t)result;
            if ((uint32_t)result < amount) break;
        }
        return offset;
    }
    if (fd == 0u) return copy_from_user_error(HAKU_EBADF);
    if (fd != 1u && fd != 2u)
        return copy_from_user_error(HAKU_EBADF);

    while (offset < count) {
        uint32_t amount = count - offset;
        if (amount > sizeof(chunk)) amount = sizeof(chunk);
        if (!copy_from_user(chunk, user_buffer + offset, amount))
            return offset ? offset : copy_from_user_error(HAKU_EFAULT);
        for (uint32_t i = 0; i < amount; i++) {
            console_putc(chunk[i]);
            if (chunk[i] == '\n') serial_putc('\r'); /* serial is CRLF */
            serial_putc(chunk[i]);
        }
        offset += amount;
    }
    return offset;
}

static uint32_t sys_read(uint32_t fd, uint32_t user_buffer,
                         uint32_t count) {
    uint32_t copied = 0;
    uint8_t chunk[128];

    if (count > USER_IO_MAX) return copy_from_user_error(HAKU_EINVAL);
    if (count == 0)
        return fd == 0u ? 0u :
               (uint32_t)vfs_read(scheduler_current_pid(), fd, 0, 0);
    if (!paging_user_range_valid(user_buffer, count, 1))
        return copy_from_user_error(HAKU_EFAULT);
    if (fd != 0u) {
        while (copied < count) {
            uint32_t amount = count - copied;
            int32_t result;
            if (amount > sizeof(chunk)) amount = sizeof(chunk);
            result = vfs_read(scheduler_current_pid(), fd, chunk, amount);
            if (result < 0)
                return copied ? copied : (uint32_t)result;
            if (result == 0) break;
            if (!copy_to_user(user_buffer + copied, chunk,
                              (uint32_t)result))
                return copied ? copied : copy_from_user_error(HAKU_EFAULT);
            copied += (uint32_t)result;
            if ((uint32_t)result < amount) break;
        }
        return copied;
    }

    /* The PS/2 console is canonical: return after newline or count bytes. */
    while (copied < count) {
        char ch = kbd_getchar();
        if (!copy_to_user(user_buffer + copied, &ch, 1))
            return copied ? copied : copy_from_user_error(HAKU_EFAULT);
        copied++;
        if (ch == '\n') break;
    }
    return copied;
}

static uint32_t sys_open(uint32_t user_path, uint32_t flags) {
    char path[13];
    for (uint32_t i = 0; i < sizeof(path); i++) {
        if (user_path > 0xFFFFFFFFu - i ||
            !paging_user_range_valid(user_path + i, 1, 0))
            return copy_from_user_error(HAKU_EFAULT);
        path[i] = *(const char *)(user_path + i);
        if (!path[i]) {
            int32_t result = vfs_open(scheduler_current_pid(), path, flags);
            return (uint32_t)result;
        }
    }
    return copy_from_user_error(HAKU_ENAMETOOLONG);
}

/* Copy a user path into a 13-byte kernel buffer. Returns 1 on success,
 * otherwise a negative -HAKU_E* errno ready to return to the caller. */
static int copy_user_path(uint32_t user_path, char out[13]) {
    for (uint32_t i = 0; i < 13u; i++) {
        if (user_path > 0xFFFFFFFFu - i ||
            !paging_user_range_valid(user_path + i, 1, 0))
            return -(int32_t)HAKU_EFAULT;
        out[i] = *(const char *)(user_path + i);
        if (!out[i]) return 1;
    }
    return -(int32_t)HAKU_ENAMETOOLONG;
}

/* POSIX returns 0 on success here: translate the VFS-internal 1. */
static uint32_t sys_unlink(uint32_t user_path) {
    char path[13];
    int32_t result;
    int copied = copy_user_path(user_path, path);
    if (copied != 1) return (uint32_t)copied;
    result = vfs_unlink(scheduler_current_pid(), path);
    return result == 1 ? 0u : (uint32_t)result;
}

static uint32_t sys_rename(uint32_t user_old, uint32_t user_new) {
    char oldpath[13];
    char newpath[13];
    int32_t result;
    int copied_old = copy_user_path(user_old, oldpath);
    int copied_new = copy_user_path(user_new, newpath);
    if (copied_old != 1) return (uint32_t)copied_old;
    if (copied_new != 1) return (uint32_t)copied_new;
    result = vfs_rename(scheduler_current_pid(), oldpath, newpath);
    return result == 1 ? 0u : (uint32_t)result;
}

static uint32_t sys_stat(uint32_t user_path, uint32_t user_buf) {
    char path[13];
    vfs_stat_t st;
    int32_t result;
    int copied = copy_user_path(user_path, path);
    if (copied != 1) return (uint32_t)copied;
    if (!paging_user_range_valid(user_buf, sizeof(st), 1))
        return copy_from_user_error(HAKU_EFAULT);
    result = vfs_stat(scheduler_current_pid(), path, &st);
    if (result != 1) return (uint32_t)result;
    if (!copy_to_user(user_buf, &st, sizeof(st)))
        return copy_from_user_error(HAKU_EFAULT);
    return 0;
}

static uint32_t sys_fstat(uint32_t fd, uint32_t user_buf) {
    vfs_stat_t st;
    int32_t result;
    if (!paging_user_range_valid(user_buf, sizeof(st), 1))
        return copy_from_user_error(HAKU_EFAULT);
    result = vfs_fstat(scheduler_current_pid(), fd, &st);
    if (result != 1) return (uint32_t)result;
    if (!copy_to_user(user_buf, &st, sizeof(st)))
        return copy_from_user_error(HAKU_EFAULT);
    return 0;
}

regs_t *user_syscall(regs_t *r) {
    switch (r->eax) {
    case SYS_EXIT:
        cpu_cli();
        klog_prefix();
        klog_write("USER-EXIT-OK status=");
        klog_dec(r->ebx);
        klog_write(" pid=");
        klog_dec(scheduler_current_pid());
        klog_write("\n");
        return scheduler_exit_current(r);
    case SYS_READ:
        r->eax = sys_read(r->ebx, r->ecx, r->edx);
        return r;
    case SYS_WRITE:
        r->eax = sys_write(r->ebx, r->ecx, r->edx);
        return r;
    case SYS_OPEN:
        r->eax = sys_open(r->ebx, r->ecx);
        return r;
    case SYS_CLOSE:
        r->eax = (uint32_t)vfs_close(scheduler_current_pid(), r->ebx);
        return r;
    case SYS_LSEEK:
        r->eax = (uint32_t)vfs_seek(scheduler_current_pid(), r->ebx,
                                    (int32_t)r->ecx, r->edx);
        return r;
    case SYS_UNLINK:
        r->eax = sys_unlink(r->ebx);
        return r;
    case SYS_RENAME:
        r->eax = sys_rename(r->ebx, r->ecx);
        return r;
    case SYS_DUP:
        r->eax = (uint32_t)vfs_dup(scheduler_current_pid(), r->ebx);
        return r;
    case SYS_DUP2:
        r->eax = (uint32_t)vfs_dup2(scheduler_current_pid(), r->ebx, r->ecx);
        return r;
    case SYS_STAT:
        r->eax = sys_stat(r->ebx, r->ecx);
        return r;
    case SYS_FSTAT:
        r->eax = sys_fstat(r->ebx, r->ecx);
        return r;
    case SYS_GETPID:
        r->eax = scheduler_current_pid();
        return r;
    default:
        r->eax = copy_from_user_error(HAKU_ENOSYS);
        return r;
    }
}
