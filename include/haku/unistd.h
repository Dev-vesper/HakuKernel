#ifndef HAKU_UNISTD_H
#define HAKU_UNISTD_H

#include <haku/types.h>
#include <haku/abi.h>

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

ssize_t read(int fd, void *buffer, size_t count);
ssize_t write(int fd, const void *buffer, size_t count);
int close(int fd);
off_t lseek(int fd, off_t offset, int whence);
int dup(int fd);
int dup2(int old_fd, int new_fd);
int unlink(const char *path);
int rename(const char *old_path, const char *new_path);
int mkdir(const char *path);
int rmdir(const char *path);
int getpid(void);
int getppid(void);
int getuid(void);
int geteuid(void);
int getgid(void);
int getegid(void);
int fork(void);
int waitpid(int pid, int *status, int options);
int spawn(const char *path);
int execve(const char *path, char *const argv[], char *const envp[]);
int getusage(haku_usage_t *usage);
int gettimeofday(haku_timeval_t *time, void *timezone);
int nanosleep(const haku_timespec_t *request, haku_timespec_t *remaining);
unsigned int sleep(unsigned int seconds);
void *sbrk(int increment);
int brk(void *address);
__declspec(noreturn) void _exit(int status);

#endif
