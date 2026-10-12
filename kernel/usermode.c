#include "usermode.h"
#include "console.h"
#include "kbd.h"
#include "paging.h"
#include "scheduler.h"

extern const uint8_t user_elf_image[];
extern const uint32_t user_elf_image_size;

static loaded_elf_t programs[2];
#define USER_PROGRAM_COUNT (sizeof(programs) / sizeof(programs[0]))

static int elf_rejection_self_test(void) {
    uint8_t invalid[116];
    loaded_elf_t rejected;

    memcpy(invalid, user_elf_image, sizeof(invalid));
    invalid[0] = 0;
    if (elf_load_executable(invalid, sizeof(invalid), &rejected)) return 0;

    memcpy(invalid, user_elf_image, sizeof(invalid));
    invalid[28] = 0xF0;
    invalid[29] = 0xFF;
    invalid[30] = 0xFF;
    invalid[31] = 0xFF;
    if (elf_load_executable(invalid, sizeof(invalid), &rejected)) return 0;

    memcpy(invalid, user_elf_image, sizeof(invalid));
    invalid[76] = 7; /* first PT_LOAD would be writable and executable */
    if (elf_load_executable(invalid, sizeof(invalid), &rejected)) return 0;

    serial_write("ELF-REJECT-OK malformed headers and W+X rejected\n");
    return 1;
}

int usermode_init(void) {
    uint32_t loaded = 0;
    if (!elf_rejection_self_test()) {
        serial_write("ELF-REJECT-FAIL\n");
        return 0;
    }
    for (; loaded < USER_PROGRAM_COUNT; loaded++) {
        if (!elf_load_executable(user_elf_image, user_elf_image_size,
                                 &programs[loaded])) {
            serial_write("ELF-LOAD-FAIL invalid or unsupported ELF32 image\n");
            while (loaded)
                paging_destroy_user_space(
                    &programs[--loaded].address_space);
            return 0;
        }
    }
    serial_write("ELF-LOAD-OK entry=");
    serial_hex(programs[0].entry);
    serial_write(" cr3=");
    serial_hex(programs[0].address_space.directory_phys);
    serial_write("\n");
    serial_write("USERMODE-READY stack=");
    serial_hex(programs[0].user_stack);
    serial_write("\n");
    if (!scheduler_init(programs, USER_PROGRAM_COUNT)) {
        serial_write("SCHED-INIT-FAIL\n");
        for (uint32_t i = 0; i < USER_PROGRAM_COUNT; i++)
            paging_destroy_user_space(&programs[i].address_space);
        return 0;
    }
    return 1;
}

void usermode_run(void) {
    serial_write("USERMODE-ENTER\n");
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
    char chunk[128];
    uint32_t offset = 0;

    if (fd != 1u && fd != 2u)
        return copy_from_user_error(HAKU_EBADF);
    if (count > USER_IO_MAX)
        return copy_from_user_error(HAKU_EINVAL);
    if (!paging_user_range_valid(user_buffer, count, 0))
        return copy_from_user_error(HAKU_EFAULT);

    while (offset < count) {
        uint32_t amount = count - offset;
        if (amount > sizeof(chunk)) amount = sizeof(chunk);
        if (!copy_from_user(chunk, user_buffer + offset, amount))
            return offset ? offset : copy_from_user_error(HAKU_EFAULT);
        for (uint32_t i = 0; i < amount; i++) {
            console_putc(chunk[i]);
            serial_putc(chunk[i]);
        }
        offset += amount;
    }
    return offset;
}

static uint32_t sys_read(uint32_t fd, uint32_t user_buffer,
                         uint32_t count) {
    uint32_t copied = 0;

    if (fd != 0u) return copy_from_user_error(HAKU_EBADF);
    if (count > USER_IO_MAX) return copy_from_user_error(HAKU_EINVAL);
    if (count == 0) return 0;
    if (!paging_user_range_valid(user_buffer, count, 1))
        return copy_from_user_error(HAKU_EFAULT);

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

regs_t *user_syscall(regs_t *r) {
    switch (r->eax) {
    case SYS_EXIT:
        cpu_cli();
        serial_write("USER-EXIT-OK status=");
        serial_dec(r->ebx);
        serial_write(" pid=");
        serial_dec(scheduler_current_pid());
        serial_write("\n");
        return scheduler_exit_current(r);
    case SYS_READ:
        r->eax = sys_read(r->ebx, r->ecx, r->edx);
        return r;
    case SYS_WRITE:
        r->eax = sys_write(r->ebx, r->ecx, r->edx);
        return r;
    case SYS_GETPID:
        r->eax = scheduler_current_pid();
        return r;
    default:
        r->eax = copy_from_user_error(HAKU_ENOSYS);
        return r;
    }
}
