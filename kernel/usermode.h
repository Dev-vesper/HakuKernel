/* i386 POSIX-inspired syscall ABI for user processes. */
#ifndef HAKU_USERMODE_H
#define HAKU_USERMODE_H

#include "idt.h"
#include "elf.h"
#include "haku/abi.h"

#define USER_CODE_BASE  0x40000000u
#define USER_STACK_TOP  0xC0000000u
#define USER_PAGE_SIZE  0x1000u
#define USER_IO_MAX     4096u

int usermode_init(void);
void usermode_run(void);
regs_t *user_syscall(regs_t *r);

#endif
