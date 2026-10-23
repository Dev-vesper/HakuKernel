#ifndef HAKU_DIRENT_H
#define HAKU_DIRENT_H

#include <haku/types.h>

/* The Linux i386 linux_dirent64 record. Haku stores 8.3 names, so
 * d_name never exceeds 12 characters plus the NUL, but the structure is
 * the one musl and busybox expect. */
struct dirent {
    unsigned long long d_ino;
    long long d_off;
    unsigned short d_reclen;
    unsigned char d_type;
    char d_name[256];
};

typedef struct {
    int fd;
    int length;
    int position;
    struct dirent entry;
    char buffer[512];
} DIR;

/* Raw syscall wrapper: fills buffer with packed records and returns the
 * number of bytes written (0 at the end of the directory). */
int getdents64(int fd, void *buffer, size_t count);

/* The minimal directory API built on getdents64. opendir returns NULL and
 * sets errno on failure; readdir returns NULL at the end of the
 * directory. */
DIR *opendir(const char *path);
struct dirent *readdir(DIR *dir);
int closedir(DIR *dir);

#endif
