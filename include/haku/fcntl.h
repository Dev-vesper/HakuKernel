#ifndef HAKU_FCNTL_H
#define HAKU_FCNTL_H

#include <haku/abi.h>

#define O_RDONLY HAKU_O_RDONLY
#define O_WRONLY HAKU_O_WRONLY
#define O_RDWR   HAKU_O_RDWR
#define O_ACCMODE HAKU_O_ACCMODE
#define O_CREAT  HAKU_O_CREAT
#define O_EXCL   HAKU_O_EXCL
#define O_TRUNC  HAKU_O_TRUNC
#define O_APPEND HAKU_O_APPEND

int open(const char *path, int flags, ...);

#endif
