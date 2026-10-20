#include <haku/errno.h>
#include <haku/fcntl.h>
#include <haku/stat.h>
#include <haku/syscall.h>
#include <haku/unistd.h>

ssize_t read(int fd, void *buffer, size_t count) {
    return haku_errno_check(
        haku_syscall3(HAKU_SYS_READ, fd, (int)buffer, (int)count));
}

ssize_t write(int fd, const void *buffer, size_t count) {
    return haku_errno_check(
        haku_syscall3(HAKU_SYS_WRITE, fd, (int)buffer, (int)count));
}

int close(int fd) {
    return haku_errno_check(haku_syscall1(HAKU_SYS_CLOSE, fd));
}

off_t lseek(int fd, off_t offset, int whence) {
    return haku_errno_check(
        haku_syscall3(HAKU_SYS_LSEEK, fd, (int)offset, whence));
}

int dup(int fd) {
    return haku_errno_check(haku_syscall1(HAKU_SYS_DUP, fd));
}

int dup2(int old_fd, int new_fd) {
    return haku_errno_check(
        haku_syscall2(HAKU_SYS_DUP2, old_fd, new_fd));
}

int open(const char *path, int flags, ...) {
    return haku_errno_check(
        haku_syscall2(HAKU_SYS_OPEN, (int)path, flags));
}

int unlink(const char *path) {
    return haku_errno_check(haku_syscall1(HAKU_SYS_UNLINK, (int)path));
}

int rename(const char *old_path, const char *new_path) {
    return haku_errno_check(
        haku_syscall2(HAKU_SYS_RENAME, (int)old_path, (int)new_path));
}

int mkdir(const char *path) {
    return haku_errno_check(haku_syscall1(HAKU_SYS_MKDIR, (int)path));
}

int rmdir(const char *path) {
    return haku_errno_check(haku_syscall1(HAKU_SYS_RMDIR, (int)path));
}

int getpid(void) {
    return haku_syscall0(HAKU_SYS_GETPID);
}

int waitpid(int pid, int *status, int options) {
    return haku_errno_check(
        haku_syscall3(HAKU_SYS_WAITPID, pid, (int)status, options));
}

int spawn(const char *path) {
    return haku_errno_check(haku_syscall1(HAKU_SYS_SPAWN, (int)path));
}

int execve(const char *path, char *const argv[], char *const envp[]) {
    return haku_errno_check(
        haku_syscall3(HAKU_SYS_EXECVE, (int)path, (int)argv, (int)envp));
}

int getusage(haku_usage_t *usage) {
    return haku_errno_check(
        haku_syscall1(HAKU_SYS_GETUSAGE, (int)usage));
}

int gettimeofday(haku_timeval_t *time, void *timezone) {
    return haku_errno_check(
        haku_syscall2(HAKU_SYS_GETTIMEOFDAY, (int)time, (int)timezone));
}

int nanosleep(const haku_timespec_t *request, haku_timespec_t *remaining) {
    return haku_errno_check(
        haku_syscall2(HAKU_SYS_NANOSLEEP, (int)request, (int)remaining));
}

unsigned int sleep(unsigned int seconds) {
    haku_timespec_t request;
    request.tv_sec = seconds;
    request.tv_nsec = 0;
    if (nanosleep(&request, NULL) != 0) return seconds;
    return 0;
}

void *sbrk(int increment) {
    int current = haku_syscall1(HAKU_SYS_BRK, 0);
    int next;
    if (current < 0) {
        errno = -current;
        return (void *)-1;
    }
    next = haku_syscall1(HAKU_SYS_BRK, current + increment);
    if (next < 0) {
        errno = -next;
        return (void *)-1;
    }
    return (void *)current;
}

int brk(void *address) {
    int result = haku_syscall1(HAKU_SYS_BRK, (int)address);
    if (result < 0) {
        errno = -result;
        return -1;
    }
    return 0;
}

int stat(const char *path, haku_stat_t *status) {
    return haku_errno_check(
        haku_syscall2(HAKU_SYS_STAT, (int)path, (int)status));
}

int fstat(int fd, haku_stat_t *status) {
    return haku_errno_check(
        haku_syscall2(HAKU_SYS_FSTAT, fd, (int)status));
}

void _exit(int status) {
    haku_syscall1(HAKU_SYS_EXIT, status);
    for (;;) {
    }
}

void exit(int status) {
    _exit(status);
}
