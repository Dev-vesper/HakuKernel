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

static uint32_t alloc_page_table(void) {
    void *frame = frame_alloc();
    if (!frame) return 0;
    if ((uint32_t)frame >= PAGING_IDMAP_MB * 1024u * 1024u) {
        frame_free(frame);
        return 0;
    }
    memset(frame, 0, 4096);
    return (uint32_t)frame;
}

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

int paging_create_user_space(address_space_t *space) {
    uint32_t *directory;
    uint32_t directory_phys;
    if (!space) return 0;
    directory_phys = alloc_page_table();
    if (!directory_phys) return 0;
    directory = (uint32_t *)directory_phys;
    directory[0] = DIR[0];
    directory[1] = DIR[1];
    space->directory_phys = directory_phys;
    return 1;
}

int paging_map_user_page(address_space_t *space, uint32_t virtual_addr,
                         uint32_t physical_addr, int writable) {
    uint32_t *directory;
    uint32_t directory_index;
    uint32_t table_index;
    uint32_t table_phys;
    uint32_t flags = PAGE_PRESENT | PAGE_USER;

    if (!space || !space->directory_phys ||
        (virtual_addr & 0xFFFu) != 0 ||
        (physical_addr & 0xFFFu) != 0 ||
        virtual_addr < 0x40000000u ||
        virtual_addr >= PAGING_USER_LIMIT ||
        physical_addr >= PAGING_IDMAP_MB * 1024u * 1024u)
        return 0;
    if (writable) flags |= PAGE_RW;

    directory = (uint32_t *)space->directory_phys;
    directory_index = virtual_addr >> 22;
    table_index = (virtual_addr >> 12) & 0x3FFu;
    if (!(directory[directory_index] & PAGE_PRESENT)) {
        table_phys = alloc_page_table();
        if (!table_phys) return 0;
        directory[directory_index] = table_phys | PAGE_PRESENT |
                                     PAGE_RW | PAGE_USER;
    } else {
        if (!(directory[directory_index] & PAGE_USER)) return 0;
        table_phys = directory[directory_index] & 0xFFFFF000u;
        if (table_phys >= PAGING_IDMAP_MB * 1024u * 1024u) return 0;
    }

    if (((uint32_t *)table_phys)[table_index] & PAGE_PRESENT) return 0;
    ((uint32_t *)table_phys)[table_index] =
        (physical_addr & 0xFFFFF000u) | flags;
    return 1;
}

void paging_activate_user_space(const address_space_t *space) {
    if (!space || !space->directory_phys) return;
    __asm {
        mov eax, space
        mov eax, [eax]
        mov cr3, eax
    }
}

void paging_activate_kernel_space(void) {
    __asm {
        mov eax, PAGING_DIR_PHYS
        mov cr3, eax
    }
}

void paging_destroy_user_space(address_space_t *space) {
    uint32_t *directory;
    if (!space || !space->directory_phys ||
        space->directory_phys == PAGING_DIR_PHYS)
        return;

    directory = (uint32_t *)space->directory_phys;
    for (uint32_t di = 256; di < (PAGING_USER_LIMIT >> 22); di++) {
        uint32_t pde = directory[di];
        if (!(pde & PAGE_PRESENT)) continue;
        {
            uint32_t table_phys = pde & 0xFFFFF000u;
            uint32_t *table;
            if (table_phys >= PAGING_IDMAP_MB * 1024u * 1024u) continue;
            table = (uint32_t *)table_phys;
            for (uint32_t ti = 0; ti < PAGING_ENTRIES; ti++) {
                if (table[ti] & PAGE_PRESENT)
                    frame_free((void *)(table[ti] & 0xFFFFF000u));
            }
            frame_free((void *)table_phys);
        }
    }
    frame_free((void *)space->directory_phys);
    space->directory_phys = 0;
}

int paging_user_range_valid(uint32_t virtual_addr, uint32_t length,
                            int write_access) {
    uint32_t last_addr;
    uint32_t page_addr;
    uint32_t directory_phys = paging_get_cr3() & 0xFFFFF000u;
    uint32_t *directory;

    if (length == 0) return 1;
    if (virtual_addr < 0x40000000u ||
        virtual_addr >= PAGING_USER_LIMIT ||
        length - 1u > 0xFFFFFFFFu - virtual_addr)
        return 0;
    last_addr = virtual_addr + length - 1u;
    if (last_addr >= PAGING_USER_LIMIT) return 0;
    if (directory_phys >= PAGING_IDMAP_MB * 1024u * 1024u) return 0;
    directory = (uint32_t *)directory_phys;

    page_addr = virtual_addr & 0xFFFFF000u;
    for (;;) {
        uint32_t pde = directory[page_addr >> 22];
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

int paging_copy_to_user_space(const address_space_t *space,
                              uint32_t virtual_addr, const void *source,
                              uint32_t length) {
    const uint8_t *input = (const uint8_t *)source;
    uint32_t directory_phys;
    uint32_t copied = 0;
    uint32_t end;
    uint32_t *directory;

    if (!space || !space->directory_phys || (!source && length) ||
        virtual_addr < 0x40000000u ||
        virtual_addr >= PAGING_USER_LIMIT ||
        length > PAGING_USER_LIMIT - virtual_addr)
        return 0;
    if (!length) return 1;

    directory_phys = space->directory_phys;
    if (directory_phys >= PAGING_IDMAP_MB * 1024u * 1024u) return 0;
    directory = (uint32_t *)directory_phys;
    end = virtual_addr + length;

    while (virtual_addr < end) {
        uint32_t pde = directory[virtual_addr >> 22];
        uint32_t table_phys;
        uint32_t pte;
        uint32_t page_offset = virtual_addr & 0xFFFu;
        uint32_t amount = 0x1000u - page_offset;

        if ((pde & (PAGE_PRESENT | PAGE_USER | PAGE_RW)) !=
            (PAGE_PRESENT | PAGE_USER | PAGE_RW))
            return 0;
        table_phys = pde & 0xFFFFF000u;
        if (table_phys >= PAGING_IDMAP_MB * 1024u * 1024u) return 0;
        pte = ((uint32_t *)table_phys)[(virtual_addr >> 12) & 0x3FFu];
        if ((pte & (PAGE_PRESENT | PAGE_USER | PAGE_RW)) !=
            (PAGE_PRESENT | PAGE_USER | PAGE_RW))
            return 0;
        if (amount > end - virtual_addr) amount = end - virtual_addr;
        memcpy((void *)((pte & 0xFFFFF000u) + page_offset),
               input + copied, amount);
        virtual_addr += amount;
        copied += amount;
    }
    return 1;
}
