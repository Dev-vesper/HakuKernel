/* Haku /dev header. */
#ifndef HAKU_DEVFS_H
#define HAKU_DEVFS_H

#include "cpu.h"

/* Register the backend. The caller mounts it with
 * vfs_mount("dev", "devfs", 0). */
int devfs_init(void);

#endif
