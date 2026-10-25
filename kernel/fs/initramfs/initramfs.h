/* Haku initramfs (cpio "newc") unpacker. */
#ifndef HAKU_INITRAMFS_H
#define HAKU_INITRAMFS_H

#include "cpu.h"

/* Create every archive entry under the mounted root filesystem, using
 * descriptor table 1 (the boot process, before it exists). Returns the
 * number of regular files written, or a negative -VFS_E* errno. */
int32_t initramfs_unpack(const uint8_t *archive, uint32_t size);

#endif
