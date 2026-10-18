#include "usermode.h"
#include "console.h"
#include "fat12_vfs.h"
#include "kbd.h"
#include "klog.h"
#include "paging.h"
#include "pit.h"
#include "scheduler.h"
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

/* The console behind the standard descriptors. Output mirrors the VGA text
 * console and COM1 (with CRLF on serial, as the boot log expects); input is
 * the canonical PS/2 keyboard stream, returning after a newline or count
 * bytes. It is registered with VFS, which installs it as fds 0/1/2. */
static int32_t console_read(void *buffer, uint32_t count, uint32_t *out_count) {
    uint8_t *bytes = (uint8_t *)buffer;
    uint32_t copied = 0;
    while (copied < count) {
        char ch = kbd_getchar();
        bytes[copied++] = (uint8_t)ch;
        if (ch == '\n') break;
    }
    *out_count = copied;
    return 1;
}

static int32_t console_write(const void *buffer, uint32_t count,
                             uint32_t *out_count) {
    const uint8_t *bytes = (const uint8_t *)buffer;
    for (uint32_t i = 0; i < count; i++) {
        console_putc((char)bytes[i]);
        if (bytes[i] == '\n') serial_putc('\r'); /* serial is CRLF */
        serial_putc((char)bytes[i]);
    }
    *out_count = count;
    return 1;
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
    if (!vfs_register_device(&console_device)) return 0;
    if (!vfs_register_fs(&fat12_fs_ops)) return 0;
    if (vfs_mount_root("fat12") != 1) return 0;
    image = (uint8_t *)kmalloc(USER_ELF_MAX_SIZE);
    if (!image) {
        klog_prefix();
        klog_write("ELF-DISK-FAIL: kernel heap exhausted\n");
        return 0;
    }
    read_result = fat12_read_file("HAKU.ELF", image, USER_ELF_MAX_SIZE,
                                  &image_size);
    if (read_result != 1) {
        klog_prefix();
        klog_write("ELF-DISK-FAIL: HAKU.ELF read error=");
        klog_dec((uint32_t)-read_result);
        klog_write("\n");
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
    if (!elf_load_executable(image, image_size, &initial_program)) {
        klog_prefix();
        klog_write("ELF-LOAD-FAIL: invalid or unsupported ELF32 image\n");
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
        klog_prefix();
        klog_write("SCHED-INIT-FAIL: could not start initial process\n");
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
    if (stat.size == 0 || stat.size > USER_ELF_MAX_SIZE) {
        vfs_close(pid, (uint32_t)fd);
        return -(int32_t)(stat.size ? HAKU_E2BIG : HAKU_ENOEXEC);
    }

    image = (uint8_t *)kmalloc(stat.size);
    if (!image) {
        vfs_close(pid, (uint32_t)fd);
        return -(int32_t)HAKU_ENOMEM;
    }
    while (offset < stat.size) {
        uint32_t amount = stat.size - offset;
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
    if (!elf_load_executable(image, stat.size, program)) {
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

static uint32_t sys_stat(uint32_t user_path, uint32_t user_buf) {
    char path[VFS_PATH_MAX];
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
static uint32_t sys_mmap(uint32_t user_arg) {
    uint32_t arg[HAKU_MMAP_ARG_SIZE / sizeof(uint32_t)];

    if (!copy_from_user(arg, user_arg, sizeof(arg)))
        return copy_from_user_error(HAKU_EFAULT);
    if (arg[0] != 0u ||
        (arg[2] & ~(uint32_t)(HAKU_PROT_READ | HAKU_PROT_WRITE)) != 0u ||
        arg[3] != (uint32_t)(HAKU_MAP_PRIVATE | HAKU_MAP_ANONYMOUS) ||
        arg[5] != 0u)
        return copy_from_user_error(HAKU_EINVAL);
    return scheduler_mmap(arg[1]);
}

regs_t *user_syscall(regs_t *r) {
    switch (r->eax) {
    case HAKU_SYS_EXIT:
        cpu_cli();
        klog_prefix();
        klog_write("USER-EXIT-OK status=");
        klog_dec(r->ebx);
        klog_write(" pid=");
        klog_dec(scheduler_current_pid());
        klog_write("\n");
        return scheduler_exit_current(r);
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
    case HAKU_SYS_STAT:
        r->eax = sys_stat(r->ebx, r->ecx);
        return r;
    case HAKU_SYS_FSTAT:
        r->eax = sys_fstat(r->ebx, r->ecx);
        return r;
    case HAKU_SYS_GETPID:
        r->eax = scheduler_current_pid();
        return r;
    case HAKU_SYS_GETTIMEOFDAY:
        r->eax = sys_gettimeofday(r->ebx);
        return r;
    case HAKU_SYS_NANOSLEEP:
        return sys_nanosleep(r, r->ebx, r->ecx);
    default:
        r->eax = copy_from_user_error(HAKU_ENOSYS);
        return r;
    }
}
