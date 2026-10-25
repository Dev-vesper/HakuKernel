/* Haku in-memory filesystem (ramfs).
 *
 * The userland root: long names, permission bits, timestamps, symbolic
 * links, and directories that grow with their contents. Contents live in
 * the kernel heap and disappear on reboot, which is what an initramfs
 * wants — the boot archive is unpacked into it and the system runs from
 * RAM. FAT12 stays available as the data disk for the existing 8.3 tree.
 */
#ifndef HAKU_RAMFS_H
#define HAKU_RAMFS_H

#include "cpu.h"

/* Register the backend with the VFS. Returns 1/0; the caller mounts it
 * with vfs_mount("", "ramfs", 0). */
int ramfs_init(void);

/* Node and byte accounting for /proc. Returns the number of live nodes. */
uint32_t ramfs_node_usage(uint32_t *nodes_used, uint32_t *bytes_used);

#endif
