#ifndef HAKU_STAT_H
#define HAKU_STAT_H

#include <haku/abi.h>

#define S_IFREG HAKU_S_IFREG
#define S_IFDIR HAKU_S_IFDIR
#define S_IFCHR HAKU_S_IFCHR
#define S_IFIFO HAKU_S_IFIFO
#define S_IFLNK HAKU_S_IFLNK
#define S_IFMT  HAKU_S_IFMT
#define S_ISREG(mode) HAKU_S_ISREG(mode)
#define S_ISDIR(mode) HAKU_S_ISDIR(mode)
#define S_ISLNK(mode) (((mode) & HAKU_S_IFMT) == HAKU_S_IFLNK)
#define S_ISCHR(mode) (((mode) & HAKU_S_IFMT) == HAKU_S_IFCHR)

/* stat/lstat/fstat fill the i386 "struct stat64" layout, the one glibc
 * and musl use on i386 (96 bytes). */
int stat(const char *path, haku_stat64_t *status);
int lstat(const char *path, haku_stat64_t *status);
int fstat(int fd, haku_stat64_t *status);

#endif
