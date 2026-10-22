#ifndef HAKU_MMAN_H
#define HAKU_MMAN_H

#include <haku/types.h>
#include <haku/abi.h>

/* The protection bits of mmap(2)/mprotect(2). Haku maps the pages but
 * has no separate code pages: PROT_EXEC is accepted and treated like
 * PROT_READ. */
#define PROT_NONE  HAKU_PROT_NONE
#define PROT_READ  HAKU_PROT_READ
#define PROT_WRITE HAKU_PROT_WRITE
#define PROT_EXEC  HAKU_PROT_EXEC

/* The mapping flags of mmap(2). */
#define MAP_SHARED   HAKU_MAP_SHARED
#define MAP_PRIVATE  HAKU_MAP_PRIVATE
#define MAP_FIXED    HAKU_MAP_FIXED
#define MAP_ANONYMOUS HAKU_MAP_ANONYMOUS
#define MAP_FAILED   ((void *)-1)

/* The flags of mremap(2). */
#define MREMAP_MAYMOVE HAKU_MREMAP_MAYMOVE
#define MREMAP_FIXED   HAKU_MREMAP_FIXED

/* The advice values of madvise(2). */
#define MADV_NORMAL     HAKU_MADV_NORMAL
#define MADV_RANDOM     HAKU_MADV_RANDOM
#define MADV_SEQUENTIAL HAKU_MADV_SEQUENTIAL
#define MADV_WILLNEED   HAKU_MADV_WILLNEED
#define MADV_DONTNEED   HAKU_MADV_DONTNEED
#define MADV_FREE       HAKU_MADV_FREE

void *mmap(void *address, size_t length, int prot, int flags, int fd,
           long offset);
void *mmap2(void *address, size_t length, int prot, int flags, int fd,
            long offset);
int mprotect(void *address, size_t length, int prot);
void *mremap(void *address, size_t old_length, size_t new_length, int flags,
             void *new_address);
int madvise(void *address, size_t length, int advice);
int munmap(void *address, size_t length);

/* The thread area descriptor of set_thread_area(2): the kernel reads the
 * base address from it and uses it as the user FS base. */
typedef struct {
    unsigned int entry_number;
    unsigned int base_addr;
} haku_thread_area_t;

int set_thread_area(haku_thread_area_t *area);
unsigned int get_thread_area(void);

#endif
