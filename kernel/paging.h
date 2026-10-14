/* Haku paging: i386 two-level tables, no PAE (Linux-style, simplified).
 *
 * This mirrors Linux arch/x86 early paging (head_32.S + paging_init):
 * a single page directory + one page table identity-mapping the first
 * 4 MiB, then CR3 load + CR0.PG set. Supervisor-only (U/S=0) like the
 * early Linux kernel mappings; user pages come later.
 *
 * Flag values are copied from Linux arch/x86/include/asm/pgtable_types.h
 * (_PAGE_BIT_* -> _PAGE_* masks).
 */
#ifndef HAKU_PAGING_H
#define HAKU_PAGING_H

#include "cpu.h"

/* Linux _PAGE_* masks (pgtable_types.h): present, writable, user, PWT, PCD,
 * accessed, dirty, PAT(ptime)/global(pdir), NX is bit 63 (64-bit only). */
#define PAGE_PRESENT  0x001u /* _PAGE_PRESENT  */
#define PAGE_RW       0x002u /* _PAGE_RW       */
#define PAGE_USER     0x004u /* _PAGE_USER     */
#define PAGE_PWT      0x008u /* _PAGE_PWT      */
#define PAGE_PCD      0x010u /* _PAGE_PCD      */
#define PAGE_ACCESSED 0x020u /* _PAGE_ACCESSED */
#define PAGE_DIRTY    0x040u /* _PAGE_DIRTY    */
#define PAGE_PSE      0x080u /* _PAGE_PSE (4 MiB pages, unused here) */
#define PAGE_GLOBAL   0x100u /* _PAGE_GLOBAL   */

/* CR0 / CR3 bits (Linux arch/x86/include/asm/cpufeatures + control regs). */
#define CR0_PG_BIT 0x80000000u /* X86_CR0_PG: paging enable */

/* Physical homes for the tables: free conventional RAM (above the
 * kernel at 0x10000, below the 0x90000 stack), 4 KiB-aligned by
 * construction. Two tables = first 8 MiB identity mapped, so frames
 * handed out by the frame allocator (which start at 4 MiB) are usable. */
#define PAGING_DIR_PHYS    0x20000u
#define PAGING_TABLE_PHYS  0x21000u
#define PAGING_TABLE1_PHYS 0x22000u
#define PAGING_IDMAP_MB    8u
#define PAGING_ENTRIES    1024u
#define PAGING_USER_LIMIT  0xC0000000u

typedef struct {
    uint32_t directory_phys;
} address_space_t;

void paging_init(void);        /* zero tables, install PDE0 + 1024 identity PTEs */
void paging_enable(void);      /* load CR3, set CR0.PG (call with IRQs off) */
int paging_is_enabled(void);   /* CR0.PG bit test */
uint32_t paging_get_cr3(void); /* raw CR3 (== PAGING_DIR_PHYS after enable) */
uint32_t paging_get_cr2(void); /* fault address (for the #PF handler) */
void paging_flush_all(void);   /* reload CR3: full TLB flush (Linux flush_tlb_all) */
int paging_create_user_space(address_space_t *space);
int paging_map_user_page(address_space_t *space, uint32_t virtual_addr,
                         uint32_t physical_addr, int writable);
void paging_activate_user_space(const address_space_t *space);
void paging_activate_kernel_space(void);
void paging_destroy_user_space(address_space_t *space);
int paging_user_range_valid(uint32_t virtual_addr, uint32_t length,
                            int write_access);
int paging_copy_to_user_space(const address_space_t *space,
                              uint32_t virtual_addr, const void *source,
                              uint32_t length);

#endif
