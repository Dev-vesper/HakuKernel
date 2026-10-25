/* Haku /proc header. */
#ifndef HAKU_PROCFS_H
#define HAKU_PROCFS_H

#include "cpu.h"

/* Register the backend. The caller mounts it with
 * vfs_mount("proc", "procfs", 0). */
int procfs_init(void);

#endif
