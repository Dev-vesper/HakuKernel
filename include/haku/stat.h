#ifndef HAKU_STAT_H
#define HAKU_STAT_H

#include <haku/abi.h>

#define S_IFREG HAKU_S_IFREG
#define S_IFDIR HAKU_S_IFDIR

int stat(const char *path, haku_stat_t *status);
int fstat(int fd, haku_stat_t *status);

#endif
