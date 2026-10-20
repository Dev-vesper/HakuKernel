#ifndef HAKU_ABI_H
#define HAKU_ABI_H

/* User/kernel system-call ABI for 32-bit x86.
 *
 * The syscall numbers are the i386 Linux ones (arch/x86/entry/syscalls/
 * syscall_32.tbl), so a Linux-targeted binary compiled for i386 can call the
 * kernel unchanged. Numbers are declared for the calls Haku implements today
 * and for the ones the next milestones add; the ones that are not
 * implemented yet return -ENOSYS.
 */
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
#define HAKU_SYS_MUNMAP  91u
/* i386 "newstat" family: the 64-byte struct stat layout. */
#define HAKU_SYS_STAT    106u
#define HAKU_SYS_LSTAT   107u
#define HAKU_SYS_FSTAT   108u
#define HAKU_SYS_NANOSLEEP    162u
/* i386 "stat64" family: the 96-byte struct stat64 layout that glibc and
 * musl use on i386 (musl's stat() goes through fstatat64). */
#define HAKU_SYS_STAT64  195u
#define HAKU_SYS_LSTAT64 196u
#define HAKU_SYS_FSTAT64 197u
#define HAKU_SYS_FSTATAT64 300u

/* i386 Linux syscall numbers reserved for the upcoming milestones: they
 * are listed so the ABI stays a single, Linux-shaped table, but the kernel
 * answers -ENOSYS until the matching work lands. */
#define HAKU_SYS_FORK      2u
#define HAKU_SYS_LINK      9u
#define HAKU_SYS_CHDIR     12u
#define HAKU_SYS_MKNOD     14u
#define HAKU_SYS_CHMOD     15u
#define HAKU_SYS_LCHOWN    16u
#define HAKU_SYS_MOUNT     21u
#define HAKU_SYS_UMOUNT2   52u
#define HAKU_SYS_IOCTL     54u
#define HAKU_SYS_FCNTL     55u
#define HAKU_SYS_SETUID    23u
#define HAKU_SYS_GETUID    24u
#define HAKU_SYS_ACCESS    33u
#define HAKU_SYS_KILL      37u
#define HAKU_SYS_PIPE      42u
#define HAKU_SYS_TIMES     43u
#define HAKU_SYS_SETGID    46u
#define HAKU_SYS_GETGID    47u
#define HAKU_SYS_GETEUID   49u
#define HAKU_SYS_GETEGID   50u
#define HAKU_SYS_GETPPID   64u
#define HAKU_SYS_SETRLIMIT 75u
#define HAKU_SYS_GETRUSAGE 77u
#define HAKU_SYS_SYMLINK   83u
#define HAKU_SYS_READLINK  85u
#define HAKU_SYS_REBOOT    88u
#define HAKU_SYS_TRUNCATE  92u
#define HAKU_SYS_FTRUNCATE 93u
#define HAKU_SYS_FCHMOD    94u
#define HAKU_SYS_FCHOWN    95u
#define HAKU_SYS_WAIT4     114u
#define HAKU_SYS_CLONE     120u
#define HAKU_SYS_UNAME     122u
#define HAKU_SYS_MPROTECT  125u
#define HAKU_SYS_FCHDIR    133u
#define HAKU_SYS_PERSONALITY 136u
#define HAKU_SYS_GETDENTS  141u
#define HAKU_SYS_SELECT    142u
#define HAKU_SYS_SCHED_YIELD 158u
#define HAKU_SYS_MREMAP    163u
#define HAKU_SYS_POLL      168u
#define HAKU_SYS_PRCTL     172u
#define HAKU_SYS_RT_SIGRETURN 173u
#define HAKU_SYS_RT_SIGACTION 174u
#define HAKU_SYS_RT_SIGPROCMASK 175u
#define HAKU_SYS_CHOWN     182u
#define HAKU_SYS_GETCWD    183u
#define HAKU_SYS_VFORK     190u
#define HAKU_SYS_GETRLIMIT 191u
#define HAKU_SYS_MMAP2     192u
#define HAKU_SYS_GETDENTS64 220u
#define HAKU_SYS_FCNTL64   221u
#define HAKU_SYS_GETTID    224u
#define HAKU_SYS_TKILL     238u
#define HAKU_SYS_FUTEX     240u
#define HAKU_SYS_SET_THREAD_AREA 243u
#define HAKU_SYS_EXIT_GROUP 252u
#define HAKU_SYS_EPOLL_CREATE 254u
#define HAKU_SYS_EPOLL_CTL 255u
#define HAKU_SYS_EPOLL_WAIT 256u
#define HAKU_SYS_SET_TID_ADDRESS 258u
#define HAKU_SYS_CLOCK_GETTIME 265u
#define HAKU_SYS_TGKILL    270u
#define HAKU_SYS_WAITID    284u
#define HAKU_SYS_OPENAT    295u
#define HAKU_SYS_MKDIRAT   296u
#define HAKU_SYS_FCHOWNAT  298u
#define HAKU_SYS_UNLINKAT  301u
#define HAKU_SYS_RENAMEAT  302u
#define HAKU_SYS_READLINKAT 305u
#define HAKU_SYS_FCHMODAT  306u
#define HAKU_SYS_FACCESSAT 307u
#define HAKU_SYS_SET_ROBUST_LIST 311u
#define HAKU_SYS_UTIMENSAT 320u
#define HAKU_SYS_DUP3      330u
#define HAKU_SYS_PIPE2     331u
#define HAKU_SYS_PRLIMIT64 340u
#define HAKU_SYS_GETRANDOM 355u
#define HAKU_SYS_STATX     383u
#define HAKU_SYS_RSEQ      386u

/* Haku-specific syscalls for starting a child ELF from a pathname and for
 * reading the calling process's memory/resource usage. They live outside
 * the Linux range so a Linux binary never collides with them. */
#define HAKU_SYS_SPAWN   0x1000u
#define HAKU_SYS_GETUSAGE 0x1001u


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
#define HAKU_S_IFCHR       0x2000u
#define HAKU_S_IFBLK       0x6000u
#define HAKU_S_IFIFO       0x1000u
#define HAKU_S_IFLNK       0xA000u
#define HAKU_S_IFSOCK      0xC000u
#define HAKU_S_IFMT        0xF000u
#define HAKU_S_ISREG(m)    (((m) & HAKU_S_IFMT) == HAKU_S_IFREG)
#define HAKU_S_ISDIR(m)    (((m) & HAKU_S_IFMT) == HAKU_S_IFDIR)
/* Default permissions Haku reports for FAT12 entries. */
#define HAKU_STAT_MODE_REG 0x81A4u /* S_IFREG | 0644 */
#define HAKU_STAT_MODE_DIR 0x41EDu /* S_IFDIR | 0755 */
#define HAKU_STAT_SIZE     64u
#define HAKU_STAT64_SIZE   96u
#define HAKU_STAT_BLKSIZE  512u
/* AT_FDCWD is the "relative to the current directory" dirfd value. */
#define HAKU_AT_FDCWD      (-100)
#define HAKU_AT_SYMLINK_NOFOLLOW 0x100u
#define HAKU_AT_EMPTY_PATH 0x1000u
#define HAKU_AT_NO_AUTOMOUNT 0x800u
/* statx(2) mask bits. */
#define HAKU_STATX_TYPE     0x001u
#define HAKU_STATX_MODE     0x002u
#define HAKU_STATX_NLINK    0x004u
#define HAKU_STATX_UID      0x008u
#define HAKU_STATX_GID      0x010u
#define HAKU_STATX_ATIME    0x020u
#define HAKU_STATX_MTIME    0x040u
#define HAKU_STATX_CTIME    0x080u
#define HAKU_STATX_INO      0x100u
#define HAKU_STATX_SIZE     0x200u
#define HAKU_STATX_BLOCKS   0x400u
#define HAKU_STATX_BASIC    0x7FFu

/* Time conversion factors and the fixed-width user structs used by
 * gettimeofday(2) and nanosleep(2). Values are 32-bit on this ABI. */
#define HAKU_USEC_PER_SEC  1000000u
#define HAKU_NSEC_PER_SEC  1000000000u
#define HAKU_TIMEVAL_SIZE  8u
#define HAKU_TIMESPEC_SIZE 8u

/* mmap(2) protection and mapping flags. Only anonymous private mappings
 * are supported today; the values match Linux. PROT_WRITE controls the
 * PTE R/W bit, so a PROT_READ mapping is enforced read-only. PROT_NONE
 * reservations are rejected with EINVAL. */
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
typedef unsigned short haku_u16;
typedef unsigned char haku_u8;
typedef signed int haku_i32;
typedef unsigned long long haku_u64;
typedef long long haku_i64;

/* i386 Linux "struct stat" (arch/x86/include/uapi/asm/stat.h): the layout
 * returned by stat(106), lstat(107), and fstat(108). 64 bytes. */
typedef struct {
    haku_u32 st_dev;
    haku_u32 st_ino;
    haku_u16 st_mode;
    haku_u16 st_nlink;
    haku_u16 st_uid;
    haku_u16 st_gid;
    haku_u32 st_rdev;
    haku_u32 st_size;
    haku_u32 st_blksize;
    haku_u32 st_blocks;
    haku_u32 st_atime;
    haku_u32 st_atime_nsec;
    haku_u32 st_mtime;
    haku_u32 st_mtime_nsec;
    haku_u32 st_ctime;
    haku_u32 st_ctime_nsec;
    haku_u32 __unused4;
    haku_u32 __unused5;
} haku_stat_t;

/* i386 Linux "struct stat64" (uapi/asm/stat.h): the glibc2.1-compatible
 * layout returned by stat64(195), lstat64(196), fstat64(197), and
 * fstatat64(300). This is what glibc and musl expect on i386, so it is the
 * layout the user library uses. The 4-byte packing reproduces the i386
 * System V ABI, where long long members are only 4-byte aligned. */
#pragma pack(push, 4)
typedef struct {
    haku_u64 st_dev;
    haku_u8  __pad0[4];
    haku_u32 __st_ino;
    haku_u32 st_mode;
    haku_u32 st_nlink;
    haku_u32 st_uid;
    haku_u32 st_gid;
    haku_u64 st_rdev;
    haku_u8  __pad3[4];
    haku_i64 st_size;
    haku_u32 st_blksize;
    haku_u64 st_blocks;
    haku_u32 st_atime;
    haku_u32 st_atime_nsec;
    haku_u32 st_mtime;
    haku_u32 st_mtime_nsec;
    haku_u32 st_ctime;
    haku_u32 st_ctime_nsec;
    haku_u64 st_ino;
} haku_stat64_t;
#pragma pack(pop)

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

/* getusage(0x1001) snapshot of the calling process. Ticks are 100 Hz timer
 * ticks; pages are 4 KiB user pages, page tables, and kernel-stack pages.
 * resident_bytes covers mapped user pages only, kernel_bytes covers the
 * user page tables plus the directory and kernel stack, and total_bytes is
 * their sum. peak_* are process-lifetime high-water marks. */
#define HAKU_USAGE_SIZE 64u

typedef struct {
    haku_u32 pid;                 /*  0 */
    haku_u32 user_ticks;          /*  4 */
    haku_u32 kernel_ticks;        /*  8 */
    haku_u32 syscalls;            /* 12 */
    haku_u32 voluntary_switches;  /* 16 */
    haku_u32 involuntary_switches; /* 20 */
    haku_u32 mapped_pages;        /* 24 */
    haku_u32 peak_pages;          /* 28 */
    haku_u32 page_tables;         /* 32 */
    haku_u32 kernel_stack_pages;  /* 36 */
    haku_u32 heap_bytes;          /* 40 */
    haku_u32 mmap_bytes;          /* 44 */
    haku_u32 resident_bytes;      /* 48 */
    haku_u32 kernel_bytes;        /* 52 */
    haku_u32 total_bytes;         /* 56 */
    haku_u32 peak_bytes;          /* 60 */
} haku_usage_t;

/* i386 Linux "struct statx" (include/uapi/linux/stat.h): the modern
 * metadata call. musl uses statx(383) for stat() on i386 and falls back
 * to the stat64 family when the kernel does not provide it. 256 bytes. */
typedef struct {
    haku_i64 tv_sec;
    haku_u32 tv_nsec;
    haku_i32 __reserved;
} haku_statx_timestamp_t;

#pragma pack(push, 4)
typedef struct {
    haku_u32 stx_mask;
    haku_u32 stx_blksize;
    haku_u64 stx_attributes;
    haku_u32 stx_nlink;
    haku_u32 stx_uid;
    haku_u32 stx_gid;
    haku_u16 stx_mode;
    haku_u16 __pad0[1];
    haku_u64 stx_ino;
    haku_u64 stx_size;
    haku_u64 stx_blocks;
    haku_u64 stx_attributes_mask;
    haku_statx_timestamp_t stx_atime;
    haku_statx_timestamp_t stx_btime;
    haku_statx_timestamp_t stx_ctime;
    haku_statx_timestamp_t stx_mtime;
    haku_u32 stx_rdev_major;
    haku_u32 stx_rdev_minor;
    haku_u32 stx_dev_major;
    haku_u32 stx_dev_minor;
    haku_u64 stx_mnt_id;
    haku_u32 stx_dio_mem_align;
    haku_u32 stx_dio_offset_align;
    haku_u64 stx_subvol;
    haku_u32 stx_atomic_write_unit_min;
    haku_u32 stx_atomic_write_unit_max;
    haku_u32 stx_atomic_write_segments_max;
    haku_u32 __pad1[1];
    haku_u64 __pad2[9];
} haku_statx_t;
#pragma pack(pop)

typedef char haku_u32_must_be_32_bits[(sizeof(haku_u32) == 4u) ? 1 : -1];
typedef char haku_u64_must_be_64_bits[(sizeof(haku_u64) == 8u) ? 1 : -1];
typedef char haku_stat_must_be_64_bytes[
    (sizeof(haku_stat_t) == 64u) ? 1 : -1];
typedef char haku_stat64_must_be_96_bytes[
    (sizeof(haku_stat64_t) == 96u) ? 1 : -1];
typedef char haku_statx_must_be_256_bytes[
    (sizeof(haku_statx_t) == 256u) ? 1 : -1];
typedef char haku_stat_mode_must_be_16_bits[
    (sizeof(((haku_stat_t *)0)->st_mode) == 2u) ? 1 : -1];
typedef char haku_timeval_must_be_8_bytes[
    (sizeof(haku_timeval_t) == HAKU_TIMEVAL_SIZE) ? 1 : -1];
typedef char haku_timespec_must_be_8_bytes[
    (sizeof(haku_timespec_t) == HAKU_TIMESPEC_SIZE) ? 1 : -1];
typedef char haku_mmap_arg_must_be_24_bytes[
    (sizeof(haku_mmap_arg_t) == HAKU_MMAP_ARG_SIZE) ? 1 : -1];
typedef char haku_usage_must_be_64_bytes[
    (sizeof(haku_usage_t) == HAKU_USAGE_SIZE) ? 1 : -1];

#endif
