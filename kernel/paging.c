/* Haku paging implementation: Linux head_32.S logic in C.
 *
 * Linux (arch/x86/kernel/head_32.S, __startup_32) builds swapper_pg_dir
 * with PG0 identity entries, loads CR3, then sets CR0.PG. We do the same,
 * except our tables live at fixed physical 0x20000/0x21000/0x22000
 * (identity mapped, so virtual == physical and plain C pointers work
 * before and after the switch).
 */
#include "paging.h"
#include "frame.h"

#define DIR  ((uint32_t *)PAGING_DIR_PHYS)
#define TAB  ((uint32_t *)PAGING_TABLE_PHYS)
#define TAB1 ((uint32_t *)PAGING_TABLE1_PHYS)

void paging_init(void) {
    /* Fresh tables: stale TLB state cannot exist yet (PG was off), but
     * zeroing is still required — a leftover non-zero word would alias
     * a random physical page. */
    memset(DIR, 0, 4096);
    memset(TAB, 0, 4096);
    memset(TAB1, 0, 4096);
    /* PDE 0/1 -> our two page tables (present + writable, supervisor). */
    DIR[0] = (PAGING_TABLE_PHYS & 0xFFFFF000u) | PAGE_PRESENT | PAGE_RW;
    DIR[1] = (PAGING_TABLE1_PHYS & 0xFFFFF000u) | PAGE_PRESENT | PAGE_RW;
    /* 2 x 1024 x 4 KiB = first 8 MiB identity mapped: kernel (0x10000),
     * VGA (0xB8000), tables themselves, the 0x90000 stack, ISA hole, and
     * room for frames the frame allocator hands out (starts at 4 MiB). */
    for (uint32_t i = 0; i < PAGING_ENTRIES; i++) {
        TAB[i] = (i * 0x1000u) | PAGE_PRESENT | PAGE_RW;
        TAB1[i] = ((i + 1024u) * 0x1000u) | PAGE_PRESENT | PAGE_RW;
    }
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

int paging_map_user(uint32_t virtual_addr, uint32_t physical_addr) {
    uint32_t directory_index;
    uint32_t table_index;
    uint32_t *table;
    uint32_t table_phys;

    if ((virtual_addr & 0xFFFu) != 0 ||
        (physical_addr & 0xFFFu) != 0 ||
        virtual_addr >= PAGING_USER_LIMIT ||
        physical_addr >= PAGING_IDMAP_MB * 1024u * 1024u)
        return 0;

    directory_index = virtual_addr >> 22;
    table_index = (virtual_addr >> 12) & 0x3FFu;
    if (!(DIR[directory_index] & PAGE_PRESENT)) {
        void *new_table = frame_alloc();
        if (!new_table) return 0;
        table_phys = (uint32_t)new_table;
        if (table_phys >= PAGING_IDMAP_MB * 1024u * 1024u) {
            frame_free(new_table);
            return 0;
        }
        memset(new_table, 0, 4096);
        DIR[directory_index] = (table_phys & 0xFFFFF000u) |
                               PAGE_PRESENT | PAGE_RW | PAGE_USER;
    } else {
        DIR[directory_index] |= PAGE_USER;
        table_phys = DIR[directory_index] & 0xFFFFF000u;
    }

    table = (uint32_t *)table_phys;
    if (table[table_index] & PAGE_PRESENT) return 0;
    table[table_index] = (physical_addr & 0xFFFFF000u) |
                         PAGE_PRESENT | PAGE_RW | PAGE_USER;
    paging_flush_all();
    return 1;
}

int paging_user_range_valid(uint32_t virtual_addr, uint32_t length,
                            int write_access) {
    uint32_t last_addr;
    uint32_t page_addr;

    if (length == 0) return 1;
    if (virtual_addr < 0x40000000u ||
        virtual_addr >= PAGING_USER_LIMIT ||
        length - 1u > 0xFFFFFFFFu - virtual_addr)
        return 0;
    last_addr = virtual_addr + length - 1u;
    if (last_addr >= PAGING_USER_LIMIT) return 0;

    page_addr = virtual_addr & 0xFFFFF000u;
    for (;;) {
        uint32_t pde = DIR[page_addr >> 22];
        uint32_t table_phys;
        uint32_t pte;

        if ((pde & (PAGE_PRESENT | PAGE_USER)) !=
            (PAGE_PRESENT | PAGE_USER))
            return 0;
        if (write_access && !(pde & PAGE_RW)) return 0;

        table_phys = pde & 0xFFFFF000u;
        if (table_phys >= PAGING_IDMAP_MB * 1024u * 1024u)
            return 0;
        pte = ((uint32_t *)table_phys)[(page_addr >> 12) & 0x3FFu];
        if ((pte & (PAGE_PRESENT | PAGE_USER)) !=
            (PAGE_PRESENT | PAGE_USER))
            return 0;
        if (write_access && !(pte & PAGE_RW)) return 0;

        if (page_addr == (last_addr & 0xFFFFF000u)) break;
        page_addr += 0x1000u;
    }
    return 1;
}
