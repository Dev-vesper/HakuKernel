#include "gdt.h"
#include "frame.h"
#include "paging.h"

#pragma pack(push, 1)
typedef struct {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t base_mid;
    uint8_t access;
    uint8_t granularity;
    uint8_t base_high;
} gdt_entry_t;

typedef struct {
    uint16_t limit;
    uint32_t base;
} gdtr_t;

typedef struct {
    uint32_t previous;
    uint32_t esp0;
    uint16_t ss0, reserved0;
    uint32_t esp1;
    uint16_t ss1, reserved1;
    uint32_t esp2;
    uint16_t ss2, reserved2;
    uint32_t cr3, eip, eflags;
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint16_t es, reserved_es, cs, reserved_cs;
    uint16_t ss, reserved_ss, ds, reserved_ds;
    uint16_t fs, reserved_fs, gs, reserved_gs;
    uint16_t ldt, reserved_ldt;
    uint16_t trap;
    uint16_t iomap_base;
} tss_t;
#pragma pack(pop)

static gdt_entry_t gdt[6];
static gdtr_t gdtr;
static tss_t tss;

static void gdt_set(uint32_t index, uint32_t base, uint32_t limit,
                    uint8_t access, uint8_t flags) {
    gdt[index].base_low = (uint16_t)(base & 0xFFFFu);
    gdt[index].base_mid = (uint8_t)((base >> 16) & 0xFFu);
    gdt[index].base_high = (uint8_t)((base >> 24) & 0xFFu);
    gdt[index].limit_low = (uint16_t)(limit & 0xFFFFu);
    gdt[index].granularity = (uint8_t)(((limit >> 16) & 0x0Fu) |
                                        (flags & 0xF0u));
    gdt[index].access = access;
}

int gdt_init(void) {
    void *stack = frame_alloc();
    if (!stack) return 0;

    memset(gdt, 0, sizeof(gdt));
    memset(&tss, 0, sizeof(tss));
    gdt_set(1, 0, 0xFFFFFu, 0x9Au, 0xCFu); /* ring-0 code */
    gdt_set(2, 0, 0xFFFFFu, 0x92u, 0xCFu); /* ring-0 data */
    gdt_set(3, 0, 0xFFFFFu, 0xFAu, 0xCFu); /* ring-3 code */
    gdt_set(4, 0, 0xFFFFFu, 0xF2u, 0xCFu); /* ring-3 data */

    tss.ss0 = KERNEL_DS;
    tss.esp0 = (uint32_t)PHYS_TO_VIRT(stack) + FRAME_SIZE;
    tss.iomap_base = sizeof(tss);
    gdt_set(5, (uint32_t)&tss, sizeof(tss) - 1u, 0x89u, 0x00u);

    gdtr.limit = (uint16_t)(sizeof(gdt) - 1u);
    gdtr.base = (uint32_t)&gdt;
    gdt_load((uint32_t)&gdtr);
    tss_load(TSS_SELECTOR);
    return 1;
}

void gdt_set_kernel_stack(uint32_t stack_top) {
    tss.esp0 = stack_top;
}
