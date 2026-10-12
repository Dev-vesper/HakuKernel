/* First ring-3 address space and its minimal syscall ABI. */
#ifndef HAKU_USERMODE_H
#define HAKU_USERMODE_H

#include "idt.h"

#define USER_CODE_BASE  0x40000000u
#define USER_STACK_TOP  0xC0000000u
#define USER_PAGE_SIZE  0x1000u

#define SYS_EXIT  0u
#define SYS_WRITE 1u

int usermode_init(void);
void user_syscall(regs_t *r);

#endif
