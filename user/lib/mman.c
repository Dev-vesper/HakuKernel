#include <haku/errno.h>
#include <haku/mman.h>
#include <haku/syscall.h>

/* mmap(2): the old i386 call takes its arguments in a block in user
 * memory, mmap2(2) takes them in registers. Both map anonymous pages
 * only: a file-backed mapping needs a page cache, which Haku does not
 * have yet, so the descriptor is ignored. */
void *mmap2(void *address, size_t length, int prot, int flags, int fd,
            long offset) {
    int result = haku_errno_check(
        haku_syscall6(HAKU_SYS_MMAP2, (int)address, (int)length, prot, flags,
                      fd, (int)offset));
    if (result < 0) return MAP_FAILED;
    return (void *)result;
}

void *mmap(void *address, size_t length, int prot, int flags, int fd,
           long offset) {
    haku_mmap_arg_t block;
    int result;

    block.addr = (unsigned int)address;
    block.length = (unsigned int)length;
    block.prot = (unsigned int)prot;
    block.flags = (unsigned int)flags;
    block.fd = (unsigned int)fd;
    block.offset = (unsigned int)offset;
    result = haku_errno_check(
        haku_syscall1(HAKU_SYS_MMAP, (int)&block));
    if (result < 0) return MAP_FAILED;
    return (void *)result;
}

int mprotect(void *address, size_t length, int prot) {
    return haku_errno_check(
        haku_syscall3(HAKU_SYS_MPROTECT, (int)address, (int)length, prot));
}

void *mremap(void *address, size_t old_length, size_t new_length, int flags,
             void *new_address) {
    int result = haku_errno_check(
        haku_syscall5(HAKU_SYS_MREMAP, (int)address, (int)old_length,
                      (int)new_length, flags, (int)new_address));
    if (result < 0) return MAP_FAILED;
    return (void *)result;
}

int madvise(void *address, size_t length, int advice) {
    return haku_errno_check(
        haku_syscall3(HAKU_SYS_MADVISE, (int)address, (int)length, advice));
}

int munmap(void *address, size_t length) {
    return haku_errno_check(
        haku_syscall2(HAKU_SYS_MUNMAP, (int)address, (int)length));
}

int set_thread_area(haku_thread_area_t *area) {
    return haku_errno_check(
        haku_syscall1(HAKU_SYS_SET_THREAD_AREA, (int)area));
}

unsigned int get_thread_area(void) {
    return (unsigned int)haku_errno_check(
        haku_syscall0(HAKU_SYS_GET_THREAD_AREA));
}
