/* Haku E820-backed physical frame allocator.
 *
 * Two bitmaps describe up to 128 MiB of physical memory: one marks frames
 * usable according to E820, the other tracks allocation. The current direct
 * mapping only permits allocations from the first 8 MiB.
 */
#ifndef HAKU_FRAME_H
#define HAKU_FRAME_H

#include "cpu.h"

#define FRAME_SIZE 4096
#define FRAME_ENTRIES 32768                  /* 32768 frames x 4 KiB = 128 MiB */
#define FRAME_BMAP_BYTES (FRAME_ENTRIES / 8) /* 4096 bytes                    */
#define FRAME_BMAP_WORDS (FRAME_ENTRIES / 32)

/* E820 type values (Intel/SMBI spec). */
#define E820_RAM 1

/* E820 entry count left by stage2. Its 24-byte entries start at physical
 * 0x7000, which remains identity-mapped after paging is enabled. */
#define E820_COUNT (*(volatile uint32_t *)(0x6FFC))

/* Public API. */
void frame_init(void); /* call after heap_init and paging_enable */
void *frame_alloc(void);
void frame_free(void *phys);
void *frame_alloc_pages(uint32_t n);
void frame_free_pages(void *phys, uint32_t n);
uint32_t frame_total_count(void);  /* tracked physical frames (up to 128 MiB) */
uint32_t frame_usable_count(void); /* usable frames reported by E820 */
uint32_t frame_free_count(void);   /* free frames currently allocatable (< 8 MiB) */
int frame_self_test(void);

#endif
