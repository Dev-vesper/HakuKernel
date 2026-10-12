#include "usermode.h"
#include "console.h"
#include "frame.h"
#include "paging.h"

extern const uint8_t user_program_start[];
extern const uint8_t user_program_end[];

static void *alloc_identity_frame(void) {
    void *frame = frame_alloc();
    if (frame && (uint32_t)frame >= PAGING_IDMAP_MB * 1024u * 1024u) {
        frame_free(frame);
        return 0;
    }
    return frame;
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
    if (r->eax == SYS_EXIT) {
        serial_write("USER-EXIT-OK\n");
        r->eip = (uint32_t)user_mode_return;
        r->cs = KERNEL_CS;
        r->eflags = (r->eflags & 0xFFFFCFFFu) | 0x2u;
        return;
    }

    if (r->eax == SYS_WRITE) {
        uint32_t address = r->ebx;
        uint32_t end = USER_CODE_BASE + USER_PAGE_SIZE;
        char message[128];
        uint32_t count = 0;

        if (address < USER_CODE_BASE || address >= end) {
            r->eax = (uint32_t)-1;
            return;
        }
        while (count + 1u < sizeof(message) && address + count < end) {
            char ch = *(const char *)(address + count);
            if (ch == 0) {
                message[count] = 0;
                serial_write(message);
                r->eax = count;
                return;
            }
            message[count++] = ch;
        }
    }

    r->eax = (uint32_t)-1;
}
