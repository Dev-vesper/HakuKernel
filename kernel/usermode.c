#include "usermode.h"
#include "console.h"
#include "frame.h"
#include "kbd.h"
#include "paging.h"

extern const uint8_t user_program_start[];
extern const uint8_t user_program_end[];

static uint32_t current_pid = 1;

static void *alloc_identity_frame(void) {
    void *frame = frame_alloc();
    if (frame && (uint32_t)frame >= PAGING_IDMAP_MB * 1024u * 1024u) {
        frame_free(frame);
        return 0;
    }
    return frame;
}

static int copy_from_user(void *destination, uint32_t source, uint32_t length) {
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

static uint32_t syscall_error(uint32_t error) {
    return 0u - error;
}

static uint32_t sys_write(uint32_t fd, uint32_t user_buffer,
                          uint32_t count) {
    char chunk[128];
    uint32_t offset = 0;

    if (fd != 1u && fd != 2u) return syscall_error(HAKU_EBADF);
    if (count > USER_IO_MAX) return syscall_error(HAKU_EINVAL);
    if (!paging_user_range_valid(user_buffer, count, 0))
        return syscall_error(HAKU_EFAULT);

    while (offset < count) {
        uint32_t amount = count - offset;
        if (amount > sizeof(chunk)) amount = sizeof(chunk);
        if (!copy_from_user(chunk, user_buffer + offset, amount))
            return offset ? offset : syscall_error(HAKU_EFAULT);
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

    if (fd != 0u) return syscall_error(HAKU_EBADF);
    if (count > USER_IO_MAX) return syscall_error(HAKU_EINVAL);
    if (count == 0) return 0;
    if (!paging_user_range_valid(user_buffer, count, 1))
        return syscall_error(HAKU_EFAULT);

    /* The PS/2 console is canonical: return after newline or count bytes. */
    while (copied < count) {
        char ch = kbd_getchar();
        if (!copy_to_user(user_buffer + copied, &ch, 1))
            return copied ? copied : syscall_error(HAKU_EFAULT);
        copied++;
        if (ch == '\n') break;
    }
    return copied;
}

int usermode_init(void) {
    void *code = alloc_identity_frame();
    void *stack_low = alloc_identity_frame();
    void *stack_high = alloc_identity_frame();
    size_t code_size = (size_t)(user_program_end - user_program_start);

    if (!code || !stack_low || !stack_high ||
        code_size == 0 || code_size > USER_PAGE_SIZE) {
        serial_write("USERMODE-FAIL allocation or image size\n");
        return 0;
    }

    memset(code, 0, USER_PAGE_SIZE);
    memcpy(code, user_program_start, code_size);
    memset(stack_low, 0, USER_PAGE_SIZE);
    memset(stack_high, 0, USER_PAGE_SIZE);

    if (!paging_map_user(USER_CODE_BASE, (uint32_t)code) ||
        !paging_map_user(USER_STACK_TOP - 2u * USER_PAGE_SIZE,
                         (uint32_t)stack_low) ||
        !paging_map_user(USER_STACK_TOP - USER_PAGE_SIZE,
                         (uint32_t)stack_high)) {
        serial_write("USERMODE-FAIL page mapping\n");
        return 0;
    }

    serial_write("USERMODE-READY code=");
    serial_hex(USER_CODE_BASE);
    serial_write(" stack=");
    serial_hex(USER_STACK_TOP);
    serial_write("\n");
    return 1;
}

void user_syscall(regs_t *r) {
    switch (r->eax) {
    case SYS_EXIT:
        serial_write("USER-EXIT-OK status=");
        serial_dec(r->ebx);
        serial_write("\n");
        r->eip = (uint32_t)user_mode_return;
        r->cs = KERNEL_CS;
        r->eflags = 0x202u;
        return;
    case SYS_READ:
        r->eax = sys_read(r->ebx, r->ecx, r->edx);
        return;
    case SYS_WRITE:
        r->eax = sys_write(r->ebx, r->ecx, r->edx);
        return;
    case SYS_GETPID:
        r->eax = current_pid;
        return;
    default:
        r->eax = syscall_error(HAKU_ENOSYS);
        return;
    }
}
