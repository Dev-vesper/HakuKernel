/* MyOS heap: kmalloc/kfree/krealloc over a fixed identity-mapped region.
 *
 * This is a classic free-list allocator (K&R style, adapted for kernel):
 * a doubly-linked list of blocks, first-fit search, split on alloc,
 * coalesce on free. 16-byte alignment keeps MSVC happy.
 *
 * The heap lives at HEAP_START (1 MiB) .. HEAP_END (just under 4 MiB),
 * inside the 4 MiB identity map set up by paging.c. When the heap grows
 * past what we have mapped, the #PF handler fires — by design.
 */
#ifndef MYOS_HEAP_H
#define MYOS_HEAP_H

#include "cpu.h"

/* Physical bounds inside the identity-mapped 4 MiB. */
#define HEAP_START 0x00100000u /* 1 MiB — above kernel, VGA, page tables */
#define HEAP_END   0x00400000u /* 4 MiB — end of identity map           */

/* Block header: 16 bytes (two linked-list pointers + size + flags). */
typedef struct heap_block {
    struct heap_block *prev;
    struct heap_block *next;
    uint32_t size;              /* payload size in bytes (not incl. header) */
    uint32_t magic;             /* HEAP_MAGIC_FREE or HEAP_MAGIC_USED      */
} heap_block_t;

#define HEAP_MAGIC_FREE 0xDEADBEEFu
#define HEAP_MAGIC_USED 0xCAFEBABEu

/* Minimum payload: 16 bytes so the smallest block = 32 bytes total. */
#define HEAP_MIN_PAYLOAD 16u

void heap_init(void);           /* one big free block spanning [HEAP_START, HEAP_END) */
void *kmalloc(size_t size);     /* first-fit alloc, NULL on OOM */
void *kzalloc(size_t size);     /* zeroed kmalloc */
void *krealloc(void *ptr, size_t new_size);
void kfree(void *ptr);          /* NULL-safe free, coalesces neighbours */
size_t heap_free_bytes(void);   /* total free payload bytes (walks the list) */
int heap_self_test(void);       /* 1 = pass, 0 = fail (serial + VGA output) */

#endif
