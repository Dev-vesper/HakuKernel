#include "usermode.h"
#include "fat12_vfs.h"
#include "klog.h"
#include "paging.h"
#include "pit.h"
#include "scheduler.h"
#include "terminal.h"
#include "fat12.h"
#include "heap.h"
#include "vfs.h"

#define USER_ELF_MAX_SIZE (256u * 1024u)
#define USER_EXEC_MAX_ARGUMENTS 64u
#define USER_EXEC_MAX_BYTES 4096u
#define USER_STACK_BOTTOM (USER_STACK_TOP - 2u * USER_PAGE_SIZE)

static loaded_elf_t initial_program;

typedef struct {
    uint8_t *strings;
    uint32_t bytes_used;
    uint32_t argc;
    uint32_t envc;
    uint32_t argv_offsets[USER_EXEC_MAX_ARGUMENTS];
    uint32_t envp_offsets[USER_EXEC_MAX_ARGUMENTS];
} exec_arguments_t;

/* The console behind the standard descriptors. The terminal layer owns the
 * display/serial split and the keyboard input queue; this device is only the
 * VFS adapter that installs it as fds 0/1/2. */
static int32_t console_read(void *buffer, uint32_t count, uint32_t *out_count) {
    return terminal_read(buffer, count, out_count);
}

static int32_t console_write(const void *buffer, uint32_t count,
                             uint32_t *out_count) {
    return terminal_write_bytes(buffer, count, out_count);
}

static const vfs_device_ops_t console_device = {
    "console",
    console_read,
    console_write,
};

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
    int read_result;

    /* Assemble the storage stack: VFS tables, then the FAT12 backend as
     * the root filesystem. Everything below (syscalls, self-tests) only
     * speaks the vfs_* interface from here on. */
    vfs_init();
    if (!vfs_register_device(&console_device)) {
        klog_log(KLOG_ERROR, "VFS-DEVICE-FAIL: console device registration\n");
        return 0;
    }
    if (!vfs_register_fs(&fat12_fs_ops)) {
        klog_log(KLOG_ERROR, "VFS-FS-FAIL: FAT12 filesystem registration\n");
        return 0;
    }
    if (vfs_mount_root("fat12") != 1) {
        klog_log(KLOG_ERROR, "VFS-MOUNT-FAIL: FAT12 root mount\n");
        return 0;
    }
    image = (uint8_t *)kmalloc(USER_ELF_MAX_SIZE);
    if (!image) {
        klog_log(KLOG_ERROR, "ELF-DISK-FAIL: kernel heap exhausted\n");
        return 0;
    }
    read_result = fat12_read_file("HAKU.ELF", image, USER_ELF_MAX_SIZE,
                                  &image_size);
    if (read_result != 1) {
        if (klog_begin(KLOG_ERROR)) {
            klog_write("ELF-DISK-FAIL: HAKU.ELF read error=");
            klog_dec((uint32_t)-read_result);
            klog_write("\n");
        }
        kfree(image);
        return 0;
    }
    klog_prefix();
    klog_write("FAT12-READ-OK name=HAKU.ELF size=");
    klog_dec(image_size);
    klog_write("\n");

    if (!elf_rejection_self_test(image, image_size)) {
        klog_log(KLOG_ERROR, "ELF-REJECT-FAIL: bad image accepted\n");
        kfree(image);
        return 0;
    }
    if (!elf_load_executable(image, image_size, &initial_program)) {
        klog_log(KLOG_ERROR,
                 "ELF-LOAD-FAIL: invalid or unsupported ELF32 image\n");
        kfree(image);
        return 0;
    }
    kfree(image);
    klog_prefix();
    klog_write("ELF-LOAD-OK: entry=");
    klog_hex(initial_program.entry);
    klog_write(" cr3=");
    klog_hex(initial_program.address_space.directory_phys);
    klog_write("\n");
    klog_prefix();
    klog_write("USERMODE-READY: user stack=");
    klog_hex(initial_program.user_stack);
    klog_write("\n");
    if (!scheduler_init(&initial_program)) {
        klog_log(KLOG_ERROR,
                 "SCHED-INIT-FAIL: could not start initial process\n");
        paging_destroy_user_space(&initial_program.address_space);
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

static int capture_exec_vector(uint32_t user_vector, uint32_t *offsets,
                               uint32_t *count, exec_arguments_t *arguments) {
    *count = 0;
    if (!user_vector) return 1;

    for (uint32_t i = 0; i <= USER_EXEC_MAX_ARGUMENTS; i++) {
        uint32_t slot;
        uint32_t user_string;
        uint32_t string_offset = 0;
        if (i > (0xFFFFFFFFu - user_vector) / sizeof(uint32_t))
            return -(int32_t)HAKU_EFAULT;
        slot = user_vector + i * sizeof(uint32_t);
        if (!copy_from_user(&user_string, slot, sizeof(user_string)))
            return -(int32_t)HAKU_EFAULT;
        if (!user_string) return 1;
        if (i == USER_EXEC_MAX_ARGUMENTS)
            return -(int32_t)HAKU_E2BIG;

        offsets[i] = arguments->bytes_used;
        for (;;) {
            uint8_t character;
            uint32_t position = arguments->bytes_used;
            if (position >= USER_EXEC_MAX_BYTES)
                return -(int32_t)HAKU_E2BIG;
            if (string_offset > 0xFFFFFFFFu - user_string ||
                !copy_from_user(&character, user_string + string_offset, 1))
                return -(int32_t)HAKU_EFAULT;
            arguments->strings[position] = character;
            arguments->bytes_used++;
            if (!character) break;
            string_offset++;
        }
        *count = i + 1u;
    }
    return -(int32_t)HAKU_E2BIG;
}

static uint32_t place_exec_strings(loaded_elf_t *program,
                                   const exec_arguments_t *arguments,
                                   const uint32_t *offsets, uint32_t count,
                                   uint32_t *addresses,
                                   uint32_t *stack_pointer) {
    for (uint32_t i = 0; i < count; i++) {
        uint32_t offset = offsets[i];
        uint32_t length = 0;
        uint32_t address;
        while (offset + length < arguments->bytes_used &&
               arguments->strings[offset + length])
            length++;
        if (offset + length >= arguments->bytes_used)
            return HAKU_EIO;
        length++;
        if (*stack_pointer < USER_STACK_BOTTOM ||
            length > *stack_pointer - USER_STACK_BOTTOM)
            return HAKU_E2BIG;
        *stack_pointer -= length;
        address = *stack_pointer;
        if (!paging_copy_to_user_space(&program->address_space, address,
                                       arguments->strings + offset, length))
            return HAKU_EIO;
        addresses[i] = address;
    }
    return 0;
}

static uint32_t build_exec_stack(loaded_elf_t *program,
                                 const exec_arguments_t *arguments) {
    uint32_t argv_addresses[USER_EXEC_MAX_ARGUMENTS];
    uint32_t envp_addresses[USER_EXEC_MAX_ARGUMENTS];
    uint32_t words[USER_EXEC_MAX_ARGUMENTS * 2u + 5u];
    uint32_t word_count = 0;
    uint32_t stack_pointer = program->user_stack;
    uint32_t error;

    error = place_exec_strings(program, arguments, arguments->argv_offsets,
                               arguments->argc, argv_addresses,
                               &stack_pointer);
    if (error) return error;
    error = place_exec_strings(program, arguments, arguments->envp_offsets,
                               arguments->envc, envp_addresses,
                               &stack_pointer);
    if (error) return error;

    words[word_count++] = arguments->argc;
    for (uint32_t i = 0; i < arguments->argc; i++)
        words[word_count++] = argv_addresses[i];
    words[word_count++] = 0;
    for (uint32_t i = 0; i < arguments->envc; i++)
        words[word_count++] = envp_addresses[i];
    words[word_count++] = 0;
    words[word_count++] = 0; /* AT_NULL */
    words[word_count++] = 0;

    if (stack_pointer < USER_STACK_BOTTOM ||
        word_count * sizeof(uint32_t) >
            stack_pointer - USER_STACK_BOTTOM)
        return HAKU_E2BIG;
    stack_pointer = (stack_pointer - word_count * sizeof(uint32_t)) &
                    ~15u;
    if (stack_pointer < USER_STACK_BOTTOM ||
        !paging_copy_to_user_space(&program->address_space, stack_pointer,
                                   words, word_count * sizeof(uint32_t)))
        return HAKU_EIO;
    program->user_stack = stack_pointer;
    return 0;
}

static int copy_to_user(uint32_t destination, const void *source,
                        uint32_t length) {
    address_space_t *space = scheduler_current_space();
    if (!space) return 0;
    /* The generic copy resolves copy-on-write pages: a syscall that writes
     * its result into a forked stack must get a private page first. */
    return paging_copy_to_user_space(space, destination, source, length);
}

static uint32_t sys_write(uint32_t fd, uint32_t user_buffer,
                          uint32_t count) {
    uint8_t chunk[128];
    uint32_t offset = 0;

    if (count > USER_IO_MAX)
        return copy_from_user_error(HAKU_EINVAL);
    if (!paging_user_range_valid(user_buffer, count, 0))
        return copy_from_user_error(HAKU_EFAULT);
    if (count == 0)
        return (uint32_t)vfs_write(scheduler_current_pid(), fd, 0, 0);

    /* Every descriptor, including the standard 0/1/2, resolves through the
     * VFS descriptor table, so a dup2 redirection is honoured and a closed
     * standard descriptor reports EBADF like any other. */
    while (offset < count) {
        uint32_t amount = count - offset;
        int32_t result;
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

static uint32_t sys_read(uint32_t fd, uint32_t user_buffer,
                         uint32_t count) {
    uint32_t copied = 0;
    uint8_t chunk[128];

    if (count > USER_IO_MAX) return copy_from_user_error(HAKU_EINVAL);
    if (count == 0)
        return (uint32_t)vfs_read(scheduler_current_pid(), fd, 0, 0);
    if (!paging_user_range_valid(user_buffer, count, 1))
        return copy_from_user_error(HAKU_EFAULT);

    /* One path for the keyboard (fd 0) and files: the console device is
     * canonical and returns after a newline, which ends the loop early. */
    while (copied < count) {
        uint32_t amount = count - copied;
        int32_t result;
        if (amount > sizeof(chunk)) amount = sizeof(chunk);
        result = vfs_read(scheduler_current_pid(), fd, chunk, amount);
        if (result < 0)
            return copied ? copied : (uint32_t)result;
        if (result == 0) break;
        if (!copy_to_user(user_buffer + copied, chunk, (uint32_t)result))
            return copied ? copied : copy_from_user_error(HAKU_EFAULT);
        copied += (uint32_t)result;
        if ((uint32_t)result < amount) break;
    }
    return copied;
}

static uint32_t sys_open(uint32_t user_path, uint32_t flags) {
    char path[VFS_PATH_MAX];
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

/* Copy a user path into a VFS_PATH_MAX kernel buffer. Returns 1 on
 * success, otherwise a negative -HAKU_E* errno ready to return. */
static int copy_user_path(uint32_t user_path, char out[VFS_PATH_MAX]) {
    for (uint32_t i = 0; i < VFS_PATH_MAX; i++) {
        if (user_path > 0xFFFFFFFFu - i ||
            !paging_user_range_valid(user_path + i, 1, 0))
            return -(int32_t)HAKU_EFAULT;
        out[i] = *(const char *)(user_path + i);
        if (!out[i]) return 1;
    }
    return -(int32_t)HAKU_ENAMETOOLONG;
}

static int32_t load_elf_file(uint32_t pid, const char *path,
                             loaded_elf_t *program) {
    vfs_stat_t stat;
    uint8_t *image;
    uint32_t offset = 0;
    int32_t result;
    int32_t fd;

    fd = vfs_open(pid, path, VFS_O_RDONLY);
    if (fd < 0) return fd;
    result = vfs_fstat(pid, (uint32_t)fd, &stat);
    if (result != 1) {
        vfs_close(pid, (uint32_t)fd);
        return result;
    }
    if (stat.st_size == 0 || stat.st_size > USER_ELF_MAX_SIZE) {
        vfs_close(pid, (uint32_t)fd);
        return -(int32_t)(stat.st_size ? HAKU_E2BIG : HAKU_ENOEXEC);
    }

    image = (uint8_t *)kmalloc((uint32_t)stat.st_size);
    if (!image) {
        vfs_close(pid, (uint32_t)fd);
        return -(int32_t)HAKU_ENOMEM;
    }
    while (offset < stat.st_size) {
        uint32_t amount = (uint32_t)(stat.st_size - offset);
        if (amount > USER_IO_MAX) amount = USER_IO_MAX;
        result = vfs_read(pid, (uint32_t)fd, image + offset, amount);
        if (result <= 0 || (uint32_t)result > amount) {
            vfs_close(pid, (uint32_t)fd);
            kfree(image);
            return result < 0 ? result : -(int32_t)HAKU_EIO;
        }
        offset += (uint32_t)result;
    }
    result = vfs_close(pid, (uint32_t)fd);
    if (result < 0) {
        kfree(image);
        return result;
    }

    memset(program, 0, sizeof(*program));
    if (!elf_load_executable(image, (uint32_t)stat.st_size, program)) {
        kfree(image);
        return -(int32_t)HAKU_ENOEXEC;
    }
    kfree(image);
    return 0;
}

static uint32_t sys_execve(uint32_t user_path, uint32_t user_argv,
                           uint32_t user_envp, regs_t *frame) {
    char path[VFS_PATH_MAX];
    loaded_elf_t replacement;
    exec_arguments_t arguments;
    int32_t result;

    result = copy_user_path(user_path, path);
    if (result != 1) return (uint32_t)result;
    memset(&arguments, 0, sizeof(arguments));
    arguments.strings = (uint8_t *)kmalloc(USER_EXEC_MAX_BYTES);
    if (!arguments.strings)
        return copy_from_user_error(HAKU_ENOMEM);
    result = capture_exec_vector(user_argv, arguments.argv_offsets,
                                &arguments.argc, &arguments);
    if (result == 1)
        result = capture_exec_vector(user_envp, arguments.envp_offsets,
                                     &arguments.envc, &arguments);
    if (result != 1) {
        kfree(arguments.strings);
        return (uint32_t)result;
    }

    result = load_elf_file(scheduler_current_pid(), path, &replacement);
    if (result) {
        kfree(arguments.strings);
        return (uint32_t)result;
    }
    result = (int32_t)build_exec_stack(&replacement, &arguments);
    if (result != 0) {
        paging_destroy_user_space(&replacement.address_space);
        kfree(arguments.strings);
        return copy_from_user_error((uint32_t)result);
    }
    kfree(arguments.strings);
    if (!scheduler_exec_current(&replacement, frame)) {
        paging_destroy_user_space(&replacement.address_space);
        return copy_from_user_error(HAKU_EIO);
    }
    return 0;
}

static uint32_t sys_spawn(uint32_t user_path) {
    char path[VFS_PATH_MAX];
    loaded_elf_t program;
    exec_arguments_t arguments;
    int32_t result;
    int copied = copy_user_path(user_path, path);
    if (copied != 1) return (uint32_t)copied;

    memset(&arguments, 0, sizeof(arguments));
    result = load_elf_file(scheduler_current_pid(), path, &program);
    if (result) return (uint32_t)result;
    result = (int32_t)build_exec_stack(&program, &arguments);
    if (result) {
        paging_destroy_user_space(&program.address_space);
        return (uint32_t)(0u - (uint32_t)result);
    }
    result = scheduler_spawn(&program);
    if (result < 0) {
        paging_destroy_user_space(&program.address_space);
        return (uint32_t)result;
    }
    return (uint32_t)result;
}

static regs_t *sys_waitpid(regs_t *frame, uint32_t pid, uint32_t user_status,
                           uint32_t options) {
    if (options != 0) {
        frame->eax = copy_from_user_error(HAKU_EINVAL);
        return frame;
    }
    if (user_status &&
        !paging_user_range_valid(user_status, sizeof(uint32_t), 1)) {
        frame->eax = copy_from_user_error(HAKU_EFAULT);
        return frame;
    }
    return scheduler_waitpid(pid, user_status, options, frame);
}

/* POSIX returns 0 on success here: translate the VFS-internal 1. */
static uint32_t sys_unlink(uint32_t user_path) {
    char path[VFS_PATH_MAX];
    int32_t result;
    int copied = copy_user_path(user_path, path);
    if (copied != 1) return (uint32_t)copied;
    result = vfs_unlink(scheduler_current_pid(), path);
    return result == 1 ? 0u : (uint32_t)result;
}

static uint32_t sys_rename(uint32_t user_old, uint32_t user_new) {
    char oldpath[VFS_PATH_MAX];
    char newpath[VFS_PATH_MAX];
    int32_t result;
    int copied_old = copy_user_path(user_old, oldpath);
    int copied_new = copy_user_path(user_new, newpath);
    if (copied_old != 1) return (uint32_t)copied_old;
    if (copied_new != 1) return (uint32_t)copied_new;
    result = vfs_rename(scheduler_current_pid(), oldpath, newpath);
    return result == 1 ? 0u : (uint32_t)result;
}

static uint32_t sys_mkdir(uint32_t user_path) {
    char path[VFS_PATH_MAX];
    int32_t result;
    int copied = copy_user_path(user_path, path);
    if (copied != 1) return (uint32_t)copied;
    result = vfs_mkdir(scheduler_current_pid(), path);
    return result == 1 ? 0u : (uint32_t)result;
}

static uint32_t sys_rmdir(uint32_t user_path) {
    char path[VFS_PATH_MAX];
    int32_t result;
    int copied = copy_user_path(user_path, path);
    if (copied != 1) return (uint32_t)copied;
    result = vfs_rmdir(scheduler_current_pid(), path);
    return result == 1 ? 0u : (uint32_t)result;
}

/* Convert the VFS metadata into the two Linux i386 stat layouts. */
static void vfs_stat_to_stat(const vfs_stat_t *in, haku_stat_t *out) {
    out->st_dev = in->st_dev;
    out->st_ino = in->st_ino;
    out->st_mode = (haku_u16)in->st_mode;
    out->st_nlink = (haku_u16)in->st_nlink;
    out->st_uid = (haku_u16)in->st_uid;
    out->st_gid = (haku_u16)in->st_gid;
    out->st_rdev = in->st_rdev;
    out->st_size = (haku_u32)in->st_size;
    out->st_blksize = in->st_blksize;
    out->st_blocks = (haku_u32)in->st_blocks;
    out->st_atime = in->st_atime;
    out->st_atime_nsec = 0u;
    out->st_mtime = in->st_mtime;
    out->st_mtime_nsec = 0u;
    out->st_ctime = in->st_ctime;
    out->st_ctime_nsec = 0u;
    out->__unused4 = 0u;
    out->__unused5 = 0u;
}

static void vfs_stat_to_stat64(const vfs_stat_t *in, haku_stat64_t *out) {
    out->st_dev = in->st_dev;
    out->__pad0[0] = 0u;
    out->__pad0[1] = 0u;
    out->__pad0[2] = 0u;
    out->__pad0[3] = 0u;
    out->__st_ino = in->st_ino;
    out->st_mode = in->st_mode;
    out->st_nlink = in->st_nlink;
    out->st_uid = in->st_uid;
    out->st_gid = in->st_gid;
    out->st_rdev = in->st_rdev;
    out->__pad3[0] = 0u;
    out->__pad3[1] = 0u;
    out->__pad3[2] = 0u;
    out->__pad3[3] = 0u;
    out->st_size = (haku_i64)in->st_size;
    out->st_blksize = in->st_blksize;
    out->st_blocks = in->st_blocks;
    out->st_atime = in->st_atime;
    out->st_atime_nsec = 0u;
    out->st_mtime = in->st_mtime;
    out->st_mtime_nsec = 0u;
    out->st_ctime = in->st_ctime;
    out->st_ctime_nsec = 0u;
    out->st_ino = in->st_ino;
}

/* stat(106)/lstat(107): the 64-byte i386 "struct stat". Haku has no
 * symbolic links, so lstat reports the same metadata as stat. */
static uint32_t sys_stat(uint32_t user_path, uint32_t user_buf) {
    char path[VFS_PATH_MAX];
    vfs_stat_t st;
    haku_stat_t out;
    int32_t result;
    int copied = copy_user_path(user_path, path);
    if (copied != 1) return (uint32_t)copied;
    if (!paging_user_range_valid(user_buf, sizeof(out), 1))
        return copy_from_user_error(HAKU_EFAULT);
    result = vfs_stat(scheduler_current_pid(), path, &st);
    if (result != 1) return (uint32_t)result;
    vfs_stat_to_stat(&st, &out);
    if (!copy_to_user(user_buf, &out, sizeof(out)))
        return copy_from_user_error(HAKU_EFAULT);
    return 0;
}

/* stat64(195)/lstat64(196): the 96-byte i386 "struct stat64" that glibc
 * and musl use on i386. */
static uint32_t sys_stat64(uint32_t user_path, uint32_t user_buf) {
    char path[VFS_PATH_MAX];
    vfs_stat_t st;
    haku_stat64_t out;
    int32_t result;
    int copied = copy_user_path(user_path, path);
    if (copied != 1) return (uint32_t)copied;
    if (!paging_user_range_valid(user_buf, sizeof(out), 1))
        return copy_from_user_error(HAKU_EFAULT);
    result = vfs_stat(scheduler_current_pid(), path, &st);
    if (result != 1) return (uint32_t)result;
    vfs_stat_to_stat64(&st, &out);
    if (!copy_to_user(user_buf, &out, sizeof(out)))
        return copy_from_user_error(HAKU_EFAULT);
    return 0;
}

/* fork(2): the child continues at the return address with 0, the parent
 * gets the child pid. The child owns a copy of the address space and
 * inherits the open descriptors. */
static regs_t *sys_fork(regs_t *frame) {
    frame->eax = (uint32_t)scheduler_fork(frame);
    return frame;
}

/* clone(2): Haku implements the fork-like case only. The sharing flags
 * (CLONE_VM, CLONE_FILES, CLONE_THREAD, ...) need address-space sharing
 * and threads, which the kernel does not have yet. */
static regs_t *sys_clone(regs_t *frame, uint32_t flags, uint32_t stack,
                         uint32_t parent_tid, uint32_t child_tid,
                         uint32_t tls) {
    if (flags & (HAKU_CLONE_VM | HAKU_CLONE_FS | HAKU_CLONE_FILES |
                 HAKU_CLONE_SIGHAND | HAKU_CLONE_VFORK | HAKU_CLONE_THREAD)) {
        frame->eax = 0u - HAKU_EINVAL;
        return frame;
    }
    if (stack || parent_tid || child_tid || tls) {
        /* CLONE_SETTLS/CLONE_PARENT_SETTID/CLONE_CHILD_SETTID need
         * thread-local storage and a second thread: not supported. */
        frame->eax = 0u - HAKU_EINVAL;
        return frame;
    }
    return sys_fork(frame);
}

/* wait4(2): the Linux form of waitpid, with WNOHANG and a rusage block. */
static regs_t *sys_wait4(regs_t *frame, uint32_t pid, uint32_t user_status,
                         uint32_t options, uint32_t user_rusage) {
    if (user_status &&
        !paging_user_range_valid(user_status, sizeof(uint32_t), 1)) {
        frame->eax = 0u - HAKU_EFAULT;
        return frame;
    }
    if (user_rusage &&
        !paging_user_range_valid(user_rusage, sizeof(haku_rusage_t), 1)) {
        frame->eax = 0u - HAKU_EFAULT;
        return frame;
    }
    return scheduler_wait4(pid, user_status, options, user_rusage, frame);
}

/* uname(2): the kernel identifies itself; the fields are the Linux
 * utsname layout (six 65-byte strings). */
static uint32_t sys_uname(uint32_t user_buf) {
    haku_utsname_t name;
    if (!paging_user_range_valid(user_buf, sizeof(name), 1))
        return copy_from_user_error(HAKU_EFAULT);
    memset(&name, 0, sizeof(name));
    memcpy(name.sysname, "Linux", 5);
    memcpy(name.nodename, "haku", 4);
    memcpy(name.release, "0.2.0", 5);
    memcpy(name.version, "HakuKernel 0.2.0", 14);
    memcpy(name.machine, "i686", 4);
    if (!copy_to_user(user_buf, &name, sizeof(name)))
        return copy_from_user_error(HAKU_EFAULT);
    return 0;
}

/* getrusage(2): Haku tracks ticks per process but not the detailed
 * resource counters, so the block is reported as zeros. */
static uint32_t sys_getrusage(uint32_t user_buf, uint32_t who) {
    haku_rusage_t usage;
    if (who != 0) return copy_from_user_error(HAKU_EINVAL);
    if (!paging_user_range_valid(user_buf, sizeof(usage), 1))
        return copy_from_user_error(HAKU_EFAULT);
    memset(&usage, 0, sizeof(usage));
    if (!copy_to_user(user_buf, &usage, sizeof(usage)))
        return copy_from_user_error(HAKU_EFAULT);
    return 0;
}

/* times(2): the clock ticks are not exposed separately yet, so the four
 * counters are reported as zeros. */
static uint32_t sys_times(uint32_t user_buf) {
    haku_tms_t ticks;
    if (!paging_user_range_valid(user_buf, sizeof(ticks), 1))
        return copy_from_user_error(HAKU_EFAULT);
    memset(&ticks, 0, sizeof(ticks));
    if (!copy_to_user(user_buf, &ticks, sizeof(ticks)))
        return copy_from_user_error(HAKU_EFAULT);
    return 0;
}

/* fstatat64(300): the path/dirfd form musl's stat() uses. Only
 * AT_FDCWD is supported because Haku has no per-process working
 * directory; Haku paths always resolve from the root. */
static uint32_t sys_fstatat64(uint32_t dirfd, uint32_t user_path,
                              uint32_t user_buf, uint32_t flags) {
    char path[VFS_PATH_MAX];
    vfs_stat_t st;
    haku_stat64_t out;
    int32_t result;
    int copied;

    if ((int32_t)dirfd != HAKU_AT_FDCWD)
        return copy_from_user_error(HAKU_ENOSYS);
    if (flags & ~(haku_u32)HAKU_AT_SYMLINK_NOFOLLOW)
        return copy_from_user_error(HAKU_EINVAL);
    copied = copy_user_path(user_path, path);
    if (copied != 1) return (uint32_t)copied;
    if (!paging_user_range_valid(user_buf, sizeof(out), 1))
        return copy_from_user_error(HAKU_EFAULT);
    result = vfs_stat(scheduler_current_pid(), path, &st);
    if (result != 1) return (uint32_t)result;
    vfs_stat_to_stat64(&st, &out);
    if (!copy_to_user(user_buf, &out, sizeof(out)))
        return copy_from_user_error(HAKU_EFAULT);
    return 0;
}

/* statx(383): the modern metadata call. Haku answers the basic fields
 * (type, mode, ownership, size, timestamps, inode, blocks) and reports
 * the mask it filled. musl calls this for stat() on i386 and falls back
 * to the stat64 family when a kernel does not provide it. */
static uint32_t sys_statx(uint32_t dirfd, uint32_t user_path, uint32_t flags,
                          uint32_t mask, uint32_t user_buf) {
    char path[VFS_PATH_MAX];
    vfs_stat_t st;
    haku_statx_t out;
    int32_t result;
    int copied;

    if ((int32_t)dirfd != HAKU_AT_FDCWD)
        return copy_from_user_error(HAKU_ENOSYS);
    if (flags & ~(haku_u32)(HAKU_AT_SYMLINK_NOFOLLOW |
                            HAKU_AT_NO_AUTOMOUNT))
        return copy_from_user_error(HAKU_EINVAL);
    copied = copy_user_path(user_path, path);
    if (copied != 1) return (uint32_t)copied;
    if (!paging_user_range_valid(user_buf, sizeof(out), 1))
        return copy_from_user_error(HAKU_EFAULT);
    result = vfs_stat(scheduler_current_pid(), path, &st);
    if (result != 1) return (uint32_t)result;

    memset(&out, 0, sizeof(out));
    out.stx_mask = mask & HAKU_STATX_BASIC;
    out.stx_blksize = st.st_blksize;
    out.stx_nlink = st.st_nlink;
    out.stx_uid = st.st_uid;
    out.stx_gid = st.st_gid;
    out.stx_mode = (haku_u16)st.st_mode;
    out.stx_ino = st.st_ino;
    out.stx_size = st.st_size;
    out.stx_blocks = st.st_blocks;
    out.stx_atime.tv_sec = (haku_i64)st.st_atime;
    out.stx_mtime.tv_sec = (haku_i64)st.st_mtime;
    out.stx_ctime.tv_sec = (haku_i64)st.st_ctime;
    out.stx_dev_major = 1u;
    out.stx_dev_minor = 0u;
    if (!copy_to_user(user_buf, &out, sizeof(out)))
        return copy_from_user_error(HAKU_EFAULT);
    return 0;
}

/* fstat(108): the 64-byte i386 "struct stat" of an open descriptor. */
static uint32_t sys_fstat(uint32_t fd, uint32_t user_buf) {
    vfs_stat_t st;
    haku_stat_t out;
    int32_t result;
    if (!paging_user_range_valid(user_buf, sizeof(out), 1))
        return copy_from_user_error(HAKU_EFAULT);
    result = vfs_fstat(scheduler_current_pid(), fd, &st);
    if (result != 1) return (uint32_t)result;
    vfs_stat_to_stat(&st, &out);
    if (!copy_to_user(user_buf, &out, sizeof(out)))
        return copy_from_user_error(HAKU_EFAULT);
    return 0;
}

/* fstat64(197): the 96-byte i386 "struct stat64" of an open descriptor. */
static uint32_t sys_fstat64(uint32_t fd, uint32_t user_buf) {
    vfs_stat_t st;
    haku_stat64_t out;
    int32_t result;
    if (!paging_user_range_valid(user_buf, sizeof(out), 1))
        return copy_from_user_error(HAKU_EFAULT);
    result = vfs_fstat(scheduler_current_pid(), fd, &st);
    if (result != 1) return (uint32_t)result;
    vfs_stat_to_stat64(&st, &out);
    if (!copy_to_user(user_buf, &out, sizeof(out)))
        return copy_from_user_error(HAKU_EFAULT);
    return 0;
}

/* Copy the calling process's usage snapshot to a validated user buffer. */
static uint32_t sys_getusage(uint32_t user_usage) {
    haku_usage_t usage;
    if (!paging_user_range_valid(user_usage, sizeof(usage), 1))
        return copy_from_user_error(HAKU_EFAULT);
    if (scheduler_get_usage(&usage) != 0)
        return copy_from_user_error(HAKU_EINVAL);
    if (!copy_to_user(user_usage, &usage, sizeof(usage)))
        return copy_from_user_error(HAKU_EFAULT);
    return 0;
}

/* Seconds and microseconds since boot, derived from the 100 Hz tick
 * counter. A NULL tv is allowed (the caller just wants success); the
 * timezone argument is not modelled and is ignored. */
static uint32_t sys_gettimeofday(uint32_t user_tv) {
    haku_timeval_t tv;
    uint32_t ticks;
    uint32_t hz;

    if (user_tv && !paging_user_range_valid(user_tv, sizeof(tv), 1))
        return copy_from_user_error(HAKU_EFAULT);
    ticks = pit_ticks();
    hz = pit_hz();
    tv.tv_sec = ticks / hz;
    tv.tv_usec = (ticks % hz) * (HAKU_USEC_PER_SEC / hz);
    if (user_tv && !copy_to_user(user_tv, &tv, sizeof(tv)))
        return copy_from_user_error(HAKU_EFAULT);
    return 0;
}

/* Block the calling task for at least the requested interval, rounded up
 * to the next timer tick. Returns 0; there are no signals, so the sleep
 * is never interrupted and the leftover-time pointer is unused. */
static regs_t *sys_nanosleep(regs_t *frame, uint32_t user_req,
                             uint32_t user_rem) {
    haku_timespec_t req;
    uint32_t hz;
    uint32_t ns_per_tick;
    uint32_t subticks;
    uint32_t ticks;

    (void)user_rem;
    if (!copy_from_user(&req, user_req, sizeof(req))) {
        frame->eax = copy_from_user_error(HAKU_EFAULT);
        return frame;
    }
    if (req.tv_nsec >= HAKU_NSEC_PER_SEC) {
        frame->eax = copy_from_user_error(HAKU_EINVAL);
        return frame;
    }
    hz = pit_hz();
    ns_per_tick = HAKU_NSEC_PER_SEC / hz;
    subticks = (req.tv_nsec + ns_per_tick - 1u) / ns_per_tick;
    if (req.tv_sec > 0x00FFFFFFu)
        ticks = 0xFFFFFFFFu;
    else
        ticks = req.tv_sec * hz + subticks;
    return scheduler_sleep_ticks(ticks, frame);
}

/* Limited mmap(2) via the i386 old_mmap argument block. Only address 0
 * (kernel-chosen placement), read/write protection, anonymous private
 * mappings, and offset 0 are accepted; the file descriptor is ignored. */
/* Signals. The kernel keeps the pending and blocked sets in one word
 * each, so a mask is four bytes; the i386 kernel sigset_t is eight, and
 * both sizes are accepted. */
#define HAKU_SIGSET_WORDS 2u

static uint32_t sys_kill(uint32_t pid, uint32_t sig) {
    if (sig > SCHEDULER_NSIG) return copy_from_user_error(HAKU_EINVAL);
    if (pid == 0u)
        return (uint32_t)scheduler_kill_pid(scheduler_current_pid(), sig);
    /* Haku has no process groups, so -1 means every other process and any
     * other negative value is not a group Haku knows about. */
    if (pid == 0xFFFFFFFFu) return (uint32_t)scheduler_kill_all(sig);
    if (pid & 0x80000000u) return copy_from_user_error(HAKU_ESRCH);
    return (uint32_t)scheduler_kill_pid(pid, sig);
}

static uint32_t sys_tkill(uint32_t tid, uint32_t sig) {
    if (sig > SCHEDULER_NSIG) return copy_from_user_error(HAKU_EINVAL);
    return (uint32_t)scheduler_tkill(tid, sig);
}

static uint32_t sys_tgkill(uint32_t tgid, uint32_t tid, uint32_t sig) {
    if (sig > SCHEDULER_NSIG) return copy_from_user_error(HAKU_EINVAL);
    return (uint32_t)scheduler_tgkill(tgid, tid, sig);
}

static uint32_t sys_rt_sigaction(uint32_t sig, uint32_t user_action,
                                 uint32_t user_previous,
                                 uint32_t sigsetsize) {
    haku_sigaction_t action;
    haku_sigaction_t previous;
    int32_t result;

    if (sigsetsize != sizeof(uint32_t) &&
        sigsetsize != HAKU_SIGSET_WORDS * sizeof(uint32_t))
        return copy_from_user_error(HAKU_EINVAL);
    if (sig < 1u || sig > SCHEDULER_NSIG)
        return copy_from_user_error(HAKU_EINVAL);
    memset(&action, 0, sizeof(action));
    if (user_action) {
        if (!copy_from_user(&action, user_action, sizeof(action)))
            return copy_from_user_error(HAKU_EFAULT);
    }
    memset(&previous, 0, sizeof(previous));
    result = scheduler_sigaction(sig, user_action ? &action : 0, &previous);
    if (result < 0) return (uint32_t)result;
    if (user_previous) {
        if (!copy_to_user(user_previous, &previous, sizeof(previous)))
            return copy_from_user_error(HAKU_EFAULT);
    }
    return 0u;
}

static uint32_t sys_rt_sigprocmask(uint32_t how, uint32_t user_set,
                                   uint32_t user_previous,
                                   uint32_t sigsetsize) {
    uint32_t mask = 0u;
    uint32_t previous = 0u;

    if (sigsetsize != sizeof(uint32_t) &&
        sigsetsize != HAKU_SIGSET_WORDS * sizeof(uint32_t))
        return copy_from_user_error(HAKU_EINVAL);
    if (how != HAKU_SIG_BLOCK && how != HAKU_SIG_UNBLOCK &&
        how != HAKU_SIG_SETMASK)
        return copy_from_user_error(HAKU_EINVAL);
    if (user_set) {
        if (!copy_from_user(&mask, user_set, sizeof(mask)))
            return copy_from_user_error(HAKU_EFAULT);
    }
    scheduler_sigprocmask(how, mask, &previous);
    if (user_previous) {
        if (!copy_to_user(user_previous, &previous, sizeof(previous)))
            return copy_from_user_error(HAKU_EFAULT);
    }
    return 0u;
}

/* The memory syscalls. mmap(2) takes an argument block in user memory,
 * mmap2(2) takes the same values in registers; both end up here. Haku
 * maps anonymous pages only: a file-backed mapping would need a page
 * cache, which does not exist yet. */
static uint32_t sys_mmap_common(uint32_t address, uint32_t length,
                                uint32_t prot, uint32_t flags,
                                uint32_t offset) {
    if (offset != 0u) return copy_from_user_error(HAKU_EINVAL);
    return scheduler_mmap2(address, length, prot, flags);
}

static uint32_t sys_mmap(uint32_t user_arg) {
    uint32_t arg[HAKU_MMAP_ARG_SIZE / sizeof(uint32_t)];

    if (!copy_from_user(arg, user_arg, sizeof(arg)))
        return copy_from_user_error(HAKU_EFAULT);
    return sys_mmap_common(arg[0], arg[1], arg[2], arg[3], arg[5]);
}

static uint32_t sys_mmap2(uint32_t address, uint32_t length, uint32_t prot,
                          uint32_t flags, uint32_t fd, uint32_t offset) {
    /* The descriptor is ignored for an anonymous mapping, exactly like
     * Linux does, but the offset must be zero. */
    (void)fd;
    return sys_mmap_common(address, length, prot, flags, offset);
}

static uint32_t sys_mprotect(uint32_t address, uint32_t length,
                             uint32_t prot) {
    return scheduler_mprotect(address, length, prot);
}

static uint32_t sys_mremap(uint32_t old_address, uint32_t old_length,
                           uint32_t new_length, uint32_t flags,
                           uint32_t new_address) {
    return scheduler_mremap(old_address, old_length, new_length, flags,
                            new_address);
}

static uint32_t sys_madvise(uint32_t address, uint32_t length,
                            uint32_t advice) {
    return scheduler_madvise(address, length, advice);
}

/* set_thread_area(2): the user passes a pointer to the thread area
 * descriptor { entry_number, base_addr }. The base becomes the user FS
 * base (the thread pointer). */
static uint32_t sys_set_thread_area(uint32_t user_area) {
    uint32_t area[2];

    if (!copy_from_user(area, user_area, sizeof(area)))
        return copy_from_user_error(HAKU_EFAULT);
    /* Only the first thread area exists; the kernel ignores the number
     * when it is the "use the default slot" value. */
    if (area[0] != 0u && area[0] != 0xFFFFFFFFu)
        return copy_from_user_error(HAKU_EINVAL);
    if (area[1] >= 0xC0000000u) return copy_from_user_error(HAKU_EINVAL);
    return scheduler_set_thread_area(area[1]);
}

regs_t *user_syscall(regs_t *r) {
    scheduler_note_syscall();
    switch (r->eax) {
    case HAKU_SYS_EXIT:
    case HAKU_SYS_EXIT_GROUP:
        /* Haku runs one thread per process, so ending the group and ending
         * the calling thread are the same operation. */
        cpu_cli();
        klog_prefix();
        klog_write("USER-EXIT-OK status=");
        klog_dec(r->ebx);
        klog_write(" pid=");
        klog_dec(scheduler_current_pid());
        klog_write("\n");
        return scheduler_exit_current(r);
    case HAKU_SYS_FORK:
        return sys_fork(r);
    case HAKU_SYS_VFORK:
        /* The child borrows the parent's address space and the parent is
         * parked until the child execs or exits, so this call may return a
         * different task's frame (like a blocking wait). */
        return scheduler_vfork(r);
    case HAKU_SYS_CLONE:
        return sys_clone(r, r->ebx, r->ecx, r->edx, r->esi, r->edi);
    case HAKU_SYS_WAIT4:
        return sys_wait4(r, r->ebx, r->ecx, r->edx, r->esi);
    case HAKU_SYS_READ:
        r->eax = sys_read(r->ebx, r->ecx, r->edx);
        return r;
    case HAKU_SYS_WRITE:
        r->eax = sys_write(r->ebx, r->ecx, r->edx);
        return r;
    case HAKU_SYS_EXECVE:
        r->eax = sys_execve(r->ebx, r->ecx, r->edx, r);
        return r;
    case HAKU_SYS_SPAWN:
        r->eax = sys_spawn(r->ebx);
        return r;
    case HAKU_SYS_WAITPID:
        return sys_waitpid(r, r->ebx, r->ecx, r->edx);
    case HAKU_SYS_OPEN:
        r->eax = sys_open(r->ebx, r->ecx);
        return r;
    case HAKU_SYS_CLOSE:
        r->eax = (uint32_t)vfs_close(scheduler_current_pid(), r->ebx);
        return r;
    case HAKU_SYS_LSEEK:
        r->eax = (uint32_t)vfs_seek(scheduler_current_pid(), r->ebx,
                                    (int32_t)r->ecx, r->edx);
        return r;
    case HAKU_SYS_UNLINK:
        r->eax = sys_unlink(r->ebx);
        return r;
    case HAKU_SYS_RENAME:
        r->eax = sys_rename(r->ebx, r->ecx);
        return r;
    case HAKU_SYS_MKDIR:
        r->eax = sys_mkdir(r->ebx);
        return r;
    case HAKU_SYS_RMDIR:
        r->eax = sys_rmdir(r->ebx);
        return r;
    case HAKU_SYS_DUP:
        r->eax = (uint32_t)vfs_dup(scheduler_current_pid(), r->ebx);
        return r;
    case HAKU_SYS_DUP2:
        r->eax = (uint32_t)vfs_dup2(scheduler_current_pid(), r->ebx, r->ecx);
        return r;
    case HAKU_SYS_BRK:
        r->eax = scheduler_brk(r->ebx);
        return r;
    case HAKU_SYS_MMAP:
        r->eax = sys_mmap(r->ebx);
        return r;
    case HAKU_SYS_MMAP2:
        r->eax = sys_mmap2(r->ebx, r->ecx, r->edx, r->esi, r->edi, r->ebp);
        return r;
    case HAKU_SYS_MPROTECT:
        r->eax = sys_mprotect(r->ebx, r->ecx, r->edx);
        return r;
    case HAKU_SYS_MREMAP:
        r->eax = sys_mremap(r->ebx, r->ecx, r->edx, r->esi, r->edi);
        return r;
    case HAKU_SYS_MADVISE:
        r->eax = sys_madvise(r->ebx, r->ecx, r->edx);
        return r;
    case HAKU_SYS_SET_THREAD_AREA:
        r->eax = sys_set_thread_area(r->ebx);
        return r;
    case HAKU_SYS_GET_THREAD_AREA:
        r->eax = scheduler_get_thread_area();
        return r;
    case HAKU_SYS_MUNMAP:
        r->eax = scheduler_munmap(r->ebx, r->ecx);
        return r;
    case HAKU_SYS_STAT:
        r->eax = sys_stat(r->ebx, r->ecx);
        return r;
    case HAKU_SYS_LSTAT:
        r->eax = sys_stat(r->ebx, r->ecx);
        return r;
    case HAKU_SYS_FSTAT:
        r->eax = sys_fstat(r->ebx, r->ecx);
        return r;
    case HAKU_SYS_STAT64:
        r->eax = sys_stat64(r->ebx, r->ecx);
        return r;
    case HAKU_SYS_LSTAT64:
        r->eax = sys_stat64(r->ebx, r->ecx);
        return r;
    case HAKU_SYS_FSTAT64:
        r->eax = sys_fstat64(r->ebx, r->ecx);
        return r;
    case HAKU_SYS_FSTATAT64:
        r->eax = sys_fstatat64(r->ebx, r->ecx, r->edx, r->esi);
        return r;
    case HAKU_SYS_STATX:
        r->eax = sys_statx(r->ebx, r->ecx, r->edx, r->esi, r->edi);
        return r;
    case HAKU_SYS_GETPID:
        r->eax = scheduler_current_pid();
        return r;
    case HAKU_SYS_GETPPID:
        r->eax = scheduler_parent_pid();
        return r;
    case HAKU_SYS_GETTID:
        r->eax = scheduler_current_pid();
        return r;
    case HAKU_SYS_GETUID:
    case HAKU_SYS_GETEUID:
    case HAKU_SYS_GETGID:
    case HAKU_SYS_GETEGID:
        /* Haku has a single user: the root account. */
        r->eax = 0u;
        return r;
    case HAKU_SYS_UNAME:
        r->eax = sys_uname(r->ebx);
        return r;
    case HAKU_SYS_GETRUSAGE:
        r->eax = sys_getrusage(r->ebx, r->ecx);
        return r;
    case HAKU_SYS_TIMES:
        r->eax = sys_times(r->ebx);
        return r;
    case HAKU_SYS_GETTIMEOFDAY:
        r->eax = sys_gettimeofday(r->ebx);
        return r;
    case HAKU_SYS_GETUSAGE:
        r->eax = sys_getusage(r->ebx);
        return r;
    case HAKU_SYS_NANOSLEEP:
        return sys_nanosleep(r, r->ebx, r->ecx);
    case HAKU_SYS_KILL:
        r->eax = sys_kill(r->ebx, r->ecx);
        return r;
    case HAKU_SYS_TKILL:
        r->eax = sys_tkill(r->ebx, r->ecx);
        return r;
    case HAKU_SYS_TGKILL:
        r->eax = sys_tgkill(r->ebx, r->ecx, r->edx);
        return r;
    case HAKU_SYS_RT_SIGACTION:
        r->eax = sys_rt_sigaction(r->ebx, r->ecx, r->edx, r->esi);
        return r;
    case HAKU_SYS_RT_SIGPROCMASK:
        r->eax = sys_rt_sigprocmask(r->ebx, r->ecx, r->edx, r->esi);
        return r;
    case HAKU_SYS_RT_SIGRETURN:
        return scheduler_sigreturn(r);
    default:
        r->eax = copy_from_user_error(HAKU_ENOSYS);
        return r;
    }
}
