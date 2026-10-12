/* MyOS 8259 PIC driver: remap, masks and EOIs.
 *
 * Programming mirrors Linux init_8259A (arch/x86/kernel/i8259.c): mask
 * everything, ICW1..ICW4 with io_wait() delays between writes (the 8259 is,
 * in Linux's words, "a fragile beast"), then restore the saved masks.
 * FreeBSD's atpic and OpenBSD's isa PIC use the identical ICW sequence —
 * it comes straight from Intel's 8259A datasheet.
 *
 * Spurious IRQ7/IRQ15 detection follows Linux i8259A_irq_real(): read the
 * ISR through OCW3 (0x0B); if our bit isn't in service the interrupt was
 * spurious. A spurious slave IRQ gets a master-only EOI.
 */
#ifndef MYOS_PIC_H
#define MYOS_PIC_H

#include "cpu.h"

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1
#define PIC_EOI   0x20

void pic_remap(uint8_t off1, uint8_t off2);
void pic_mask_all(void);
void pic_set_mask(uint8_t irq);
void pic_clear_mask(uint8_t irq);
void pic_send_eoi(uint8_t irq);
int pic_is_spurious(uint8_t irq);

#endif
