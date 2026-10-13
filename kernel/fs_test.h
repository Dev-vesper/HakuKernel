/* Filesystem error self-test: proves every FAT12/VFS failure class keeps
 * one distinct errno end to end (missing file, invalid input, existing
 * file, bad descriptor, disk I/O, full volume). Runs once from the
 * scheduler idle task after all user processes have exited, so the
 * full-volume probe cannot disturb their file I/O tests. */
#ifndef HAKU_FS_TEST_H
#define HAKU_FS_TEST_H

void fs_error_self_test(void);

#endif
