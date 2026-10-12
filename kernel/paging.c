/* MyOS paging implementation: Linux head_32.S logic in C.
 *
 * Linux (arch/x86/kernel/head_32.S, __startup_32) builds swapper_pg_dir
 * with PG0 identity entries, loads CR3, then sets CR0.PG. We do the same,
 * except our tables live at fixed physical 0x20000/0x21000 (identity
 * mapped, so virtual == physical and plain C pointers work before and
 * after the switch).
 */
#include "paging.h"

#define DIR  ((uint32_t *)PAGING_DIR_PHYS)
#define TAB  ((uint32_t *)PAGING_TABLE_PHYS)

void paging_init(void) {
    /* Fresh tables: stale TLB state cannot exist yet (PG was off), but
     * zeroing is still required — a leftover non-zero word would alias
     * a random physical page. */
    memset(DIR, 0, 4096);
    memset(TAB, 0, 4096);
    /* PDE 0 -> our single page table (present + writable, supervisor). */
    DIR[0] = (PAGING_TABLE_PHYS & 0xFFFFF000u) | PAGE_PRESENT | PAGE_RW;
    /* 1024 x 4 KiB = first 4 MiB identity mapped: kernel (0x10000),
     * VGA (0xB8000), tables themselves, the 0x90000 stack, ISA hole. */
    for (uint32_t i = 0; i < PAGING_ENTRIES; i++)
        TAB[i] = (i * 0x1000u) | PAGE_PRESENT | PAGE_RW;
}

void paging_enable(void) {
    __asm {
        mov eax, PAGING_DIR_PHYS
        mov cr3, eax              /* MMU walks our directory from here */
        mov eax, cr0
        or eax, 080000000h        /* CR0.PG — from this insn on, every */
        mov cr0, eax              /* address goes through the tables */
    }
}

int paging_is_enabled(void) {
    /* NOTE: locals are deliberately NOT named cr0/cr3/cr2 — MASM would
     * resolve `mov eax, cr0` to the C variable instead of control
     * register CR0. `mov reg, var` reads the variable, while
     * `mov CRn, reg` (register name on the LEFT, no C shadow) writes
     * the control register. */
    uint32_t v;
    __asm {
        mov eax, cr0
        mov v, eax
    }
    return (v & CR0_PG_BIT) != 0;
}

uint32_t paging_get_cr3(void) {
    uint32_t v;
    __asm {
        mov eax, cr3
        mov v, eax
    }
    return v;
}

uint32_t paging_get_cr2(void) {
    uint32_t v;
    __asm {
        mov eax, cr2
        mov v, eax
    }
    return v;
}

void paging_flush_all(void) {
    __asm {
        mov eax, cr3
        mov cr3, eax              /* reload -> full TLB flush */
    }
}
