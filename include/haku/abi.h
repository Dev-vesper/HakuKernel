#ifndef HAKU_ABI_H
#define HAKU_ABI_H

/* User/kernel system-call ABI for 32-bit x86. */
#define HAKU_SYSCALL_VECTOR 0x80u

/* i386 Linux syscall numbers implemented by Haku. */
#define HAKU_SYS_EXIT    1u
#define HAKU_SYS_READ    3u
#define HAKU_SYS_WRITE   4u
#define HAKU_SYS_OPEN    5u
#define HAKU_SYS_CLOSE   6u
#define HAKU_SYS_WAITPID 7u
#define HAKU_SYS_UNLINK  10u
#define HAKU_SYS_EXECVE  11u
#define HAKU_SYS_LSEEK   19u
#define HAKU_SYS_GETPID  20u
#define HAKU_SYS_RENAME  38u
#define HAKU_SYS_MKDIR   39u
#define HAKU_SYS_RMDIR   40u
#define HAKU_SYS_DUP     41u
#define HAKU_SYS_BRK     45u
#define HAKU_SYS_DUP2    63u
#define HAKU_SYS_GETTIMEOFDAY 78u
/* i386 old_mmap: a single pointer to haku_mmap_arg_t in ebx. */
#define HAKU_SYS_MMAP    90u
#define HAKU_SYS_STAT    106u
#define HAKU_SYS_FSTAT   108u
#define HAKU_SYS_NANOSLEEP    162u

/* Haku-specific syscall for starting a child ELF from a pathname. */
#define HAKU_SYS_SPAWN 0x1000u

/* Raw syscall errors are returned as -errno. */
#define HAKU_E2BIG         7u
#define HAKU_ENOENT        2u
#define HAKU_EIO           5u
#define HAKU_ENOEXEC       8u
#define HAKU_EBADF          9u
#define HAKU_ECHILD        10u
#define HAKU_EAGAIN        11u
#define HAKU_ENOMEM        12u
#define HAKU_EFAULT        14u
#define HAKU_EEXIST        17u
#define HAKU_ENOTDIR       20u
#define HAKU_EISDIR        21u
#define HAKU_EINVAL        22u
#define HAKU_EMFILE        24u
#define HAKU_ENOSPC        28u
#define HAKU_EROFS         30u
#define HAKU_ENAMETOOLONG  36u
#define HAKU_ENOSYS        38u
#define HAKU_ENOTEMPTY     39u

/* open(2) flags accepted by the current VFS implementation. */
#define HAKU_O_RDONLY  0x0000u
#define HAKU_O_WRONLY  0x0001u
#define HAKU_O_RDWR    0x0002u
#define HAKU_O_ACCMODE 0x0003u
#define HAKU_O_CREAT   0x0040u
#define HAKU_O_EXCL    0x0080u
#define HAKU_O_TRUNC   0x0200u
#define HAKU_O_APPEND  0x0400u

#define HAKU_STDIN_FILENO  0u
#define HAKU_STDOUT_FILENO 1u
#define HAKU_STDERR_FILENO 2u

#define HAKU_S_IFREG       0x8000u
#define HAKU_S_IFDIR       0x4000u
#define HAKU_STAT_SIZE     8u
#define HAKU_STAT_MODE_REG 0x81A4u
#define HAKU_STAT_MODE_DIR 0x41EDu

/* Time conversion factors and the fixed-width user structs used by
 * gettimeofday(2) and nanosleep(2). Values are 32-bit on this ABI. */
#define HAKU_USEC_PER_SEC  1000000u
#define HAKU_NSEC_PER_SEC  1000000000u
#define HAKU_TIMEVAL_SIZE  8u
#define HAKU_TIMESPEC_SIZE 8u

/* mmap(2) protection and mapping flags. Only anonymous private mappings
 * are supported today; the values match Linux. */
#define HAKU_PROT_NONE  0x0u
#define HAKU_PROT_READ  0x1u
#define HAKU_PROT_WRITE 0x2u
#define HAKU_PROT_EXEC  0x4u
#define HAKU_MAP_SHARED    0x01u
#define HAKU_MAP_PRIVATE   0x02u
#define HAKU_MAP_FIXED     0x10u
#define HAKU_MAP_ANONYMOUS 0x20u
#define HAKU_MMAP_ARG_SIZE 24u

typedef unsigned int haku_u32;

/* stat/fstat user buffer layout; fields are fixed-width 32-bit values. */
typedef struct {
    haku_u32 size;
    haku_u32 mode;
} haku_stat_t;

/* gettimeofday(2): seconds + microseconds since boot. */
typedef struct {
    haku_u32 tv_sec;
    haku_u32 tv_usec;
} haku_timeval_t;

/* nanosleep(2): requested/completed duration in seconds + nanoseconds. */
typedef struct {
    haku_u32 tv_sec;
    haku_u32 tv_nsec;
} haku_timespec_t;

/* mmap(2) argument block (i386 old_mmap ABI). */
typedef struct {
    haku_u32 addr;
    haku_u32 length;
    haku_u32 prot;
    haku_u32 flags;
    haku_u32 fd;
    haku_u32 offset;
} haku_mmap_arg_t;

typedef char haku_u32_must_be_32_bits[(sizeof(haku_u32) == 4u) ? 1 : -1];
typedef char haku_stat_must_be_8_bytes[
    (sizeof(haku_stat_t) == HAKU_STAT_SIZE) ? 1 : -1];
typedef char haku_timeval_must_be_8_bytes[
    (sizeof(haku_timeval_t) == HAKU_TIMEVAL_SIZE) ? 1 : -1];
typedef char haku_timespec_must_be_8_bytes[
    (sizeof(haku_timespec_t) == HAKU_TIMESPEC_SIZE) ? 1 : -1];
typedef char haku_mmap_arg_must_be_24_bytes[
    (sizeof(haku_mmap_arg_t) == HAKU_MMAP_ARG_SIZE) ? 1 : -1];

#endif
