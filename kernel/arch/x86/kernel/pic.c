/* Haku 8259 PIC driver: remap, masks and EOIs (see pic.h for the model). */
#include "pic.h"

/* 1 bit = masked, like Linux's cached_irq_mask (starts all-masked). */
static uint8_t master_mask = 0xFF;
static uint8_t slave_mask = 0xFF;

void pic_remap(uint8_t off1, uint8_t off2) {
    outb(PIC1_DATA, 0xFF); /* mask everything while reprogramming */
    outb(PIC2_DATA, 0xFF);

    outb(PIC1_CMD, 0x11); io_wait();  /* ICW1: init, ICW4 needed */
    outb(PIC1_DATA, off1); io_wait(); /* ICW2: master vector base */
    outb(PIC1_DATA, 0x04); io_wait(); /* ICW3: slave wired to IR2 */
    outb(PIC1_DATA, 0x01); io_wait(); /* ICW4: 8086 mode */

    outb(PIC2_CMD, 0x11); io_wait();  /* ICW1: init, ICW4 needed */
    outb(PIC2_DATA, off2); io_wait(); /* ICW2: slave vector base */
    outb(PIC2_DATA, 0x02); io_wait(); /* ICW3: slave identity = cascade */
    outb(PIC2_DATA, 0x01); io_wait(); /* ICW4: 8086 mode */

    outb(PIC1_DATA, master_mask);      /* restore saved masks */
    outb(PIC2_DATA, slave_mask);
}

void pic_mask_all(void) {
    master_mask = slave_mask = 0xFF;
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
}

void pic_set_mask(uint8_t irq) {
    if (irq < 8) {
        master_mask |= (uint8_t)(1 << irq);
        outb(PIC1_DATA, master_mask);
    } else if (irq < 16) {
        slave_mask |= (uint8_t)(1 << (irq - 8));
        outb(PIC2_DATA, slave_mask);
    }
}

void pic_clear_mask(uint8_t irq) {
    if (irq < 8) {
        master_mask &= (uint8_t)~(1 << irq);
        outb(PIC1_DATA, master_mask);
    } else if (irq < 16) {
        slave_mask &= (uint8_t)~(1 << (irq - 8));
        outb(PIC2_DATA, slave_mask);
    }
}

int pic_is_spurious(uint8_t irq) {
    uint8_t isr;
    if (irq == 7) {
        outb(PIC1_CMD, 0x0B);         /* OCW3: read ISR */
        isr = inb(PIC1_CMD);
        outb(PIC1_CMD, 0x0A);         /* back to IRR */
        return !(isr & 0x80);
    }
    if (irq == 15) {
        outb(PIC2_CMD, 0x0B);
        isr = inb(PIC2_CMD);
        outb(PIC2_CMD, 0x0A);
        return !(isr & 0x80);
    }
    return 0;
}

void pic_send_eoi(uint8_t irq) {
    if (irq >= 8) {
        /* A spurious slave IRQ never really fired there: master EOI only. */
        if (!(irq == 15 && pic_is_spurious(15)))
            outb(PIC2_CMD, PIC_EOI);
        outb(PIC1_CMD, PIC_EOI);
    } else {
        outb(PIC1_CMD, PIC_EOI);      /* (also correct for spurious IRQ7) */
    }
}
