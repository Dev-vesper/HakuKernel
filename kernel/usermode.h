/* i386 POSIX-inspired syscall ABI for user processes. */
#ifndef HAKU_USERMODE_H
#define HAKU_USERMODE_H

#include "idt.h"
#include "elf.h"

#define USER_CODE_BASE  0x40000000u
#define USER_STACK_TOP  0xC0000000u
#define USER_PAGE_SIZE  0x1000u
#define USER_IO_MAX     4096u

/* i386 Linux syscall numbers for the implemented POSIX-style subset. */
#define SYS_EXIT  1u
#define SYS_OPEN  5u
#define SYS_CLOSE 6u
#define SYS_READ  3u
#define SYS_WRITE 4u
#define SYS_LSEEK 19u
#define SYS_GETPID 20u

/* Raw syscall errors are returned as -errno; a future libc sets errno. */
#define HAKU_ENOENT 2u
#define HAKU_EIO    5u
#define HAKU_EBADF  9u
#define HAKU_EFAULT 14u
#define HAKU_EEXIST 17u
#define HAKU_EINVAL 22u
#define HAKU_EMFILE 24u
#define HAKU_ENOSPC 28u
#define HAKU_ENAMETOOLONG 36u
#define HAKU_ENOSYS 38u

int usermode_init(void);
void usermode_run(void);
regs_t *user_syscall(regs_t *r);

#endif
