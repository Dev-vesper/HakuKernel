/* MyOS frame allocator interface.
 *
 * One bit per 4 KiB frame: 0 = free, 1 = used.  The bitmap (4096 bytes
 * for 32768 frames = 128 MiB) is allocated from the heap at init —
 * frame_init() must run after heap_init().
 */
#ifndef MYOS_FRAME_H
#define MYOS_FRAME_H

#include "cpu.h"

#define FRAME_SIZE 4096
#define FRAME_ENTRIES 32768                  /* 32768 frames x 4 KiB = 128 MiB */
#define FRAME_BMAP_BYTES (FRAME_ENTRIES / 8) /* 4096 bytes                    */
#define FRAME_BMAP_WORDS (FRAME_ENTRIES / 32)

/* E820 type values (Intel/SMBI spec). */
#define E820_RAM 1

/* Macros to reach the E820 map that stage2 left in low memory.
 * Paging is identity-mapped, so physical 0x7000 == virtual 0x7000. */
#define E820_COUNT (*(volatile uint32_t *)(0x6FFC))

/* Public API. */
void frame_init(void);
void *frame_alloc(void);
void frame_free(void *phys);
void *frame_alloc_pages(uint32_t n);
void frame_free_pages(void *phys, uint32_t n);
uint32_t frame_total_count(void);
uint32_t frame_free_count(void);
int frame_self_test(void);

#endif
