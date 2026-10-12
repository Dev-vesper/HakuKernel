/* Haku GDT: flat kernel/user segments and a 32-bit TSS. */
#ifndef HAKU_GDT_H
#define HAKU_GDT_H

#include "cpu.h"

#define KERNEL_DS   0x10
#define USER_DS     0x23
#define TSS_SELECTOR 0x28

int gdt_init(void);

/* Implemented in isr.asm. */
void gdt_load(uint32_t gdtr_addr);
void tss_load(uint32_t selector);

#endif
