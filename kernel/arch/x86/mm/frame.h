/* Haku E820-backed physical frame allocator.
 *
 * Two bitmaps describe the physical memory the supervisor direct map can
 * reach: one marks frames usable according to E820, the other tracks
 * allocation. The limit is detected at boot instead of being fixed, so a
 * machine with 512 MiB of RAM gets a 512 MiB frame pool; see
 * paging_phys_limit() for the 1020 MiB ceiling imposed by the 32-bit
 * address space.
 */
#ifndef HAKU_FRAME_H
#define HAKU_FRAME_H

#include "cpu.h"

#define FRAME_SIZE 4096
/* Largest pool the address-space layout allows: the direct map starts at
 * PAGING_DIRECT_MAP_BASE and runs to the top of the address space. The
 * bitmaps are allocated from the heap to the detected size, not this one. */
#define FRAME_MAX_ENTRIES (1020u * 1024u * 1024u / FRAME_SIZE)

/* E820 type values (Intel/SMBI spec). */
#define E820_RAM 1

/* E820 entry count left by stage2. Its 24-byte entries start at physical
 * 0x7000, which remains identity-mapped after paging is enabled. */
#define E820_COUNT (*(volatile uint32_t *)(0x6FFC))

/* Highest usable E820 address, clipped to the 32-bit physical range and to
 * the direct-map ceiling. paging_init uses it to size the direct map before
 * the allocator exists; 0 means the memory map is missing. */
uint32_t memmap_usable_top(void);

/* Public API. */
void frame_init(void); /* call after heap_init and paging_enable */
void *frame_alloc(void);
void frame_free(void *phys);
void *frame_alloc_pages(uint32_t n);
void frame_free_pages(void *phys, uint32_t n);
uint32_t frame_total_count(void);  /* tracked physical frames */
uint32_t frame_usable_count(void); /* usable frames reported by E820 */
uint32_t frame_free_count(void);   /* free frames currently allocatable */
uint32_t frame_phys_limit(void);   /* tracked bytes == direct-map coverage */
void frame_reserve_range(uint32_t start, uint32_t end);
int frame_self_test(void);

#endif
