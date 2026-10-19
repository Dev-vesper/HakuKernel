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

/* Reserved user virtual regions for the program break (brk) and anonymous
 * mmap, placed between the program image and the stack. They are mapped
 * lazily, one zero-filled page at a time, and released by brk shrink,
 * munmap, exit, or execve. */
#define USER_HEAP_BASE  0x50000000u
#define USER_HEAP_LIMIT 0x54000000u
#define USER_MMAP_BASE  0x60000000u
#define USER_MMAP_LIMIT 0x70000000u

int usermode_init(void);
void usermode_run(void);
regs_t *user_syscall(regs_t *r);

#endif
