/* Haku COM1 serial backend: 38400 8N1, polled TX.
 *
 * The terminal mirrors every output byte here, which keeps the boot and
 * test markers readable without a display. serial_putc is called from
 * interrupt context (klog in the timer handler), so the UART takes an
 * IRQ-save lock.
 */
#include "serial.h"
#include "sync.h"

#define COM1 0x3F8

static spinlock_t serial_lock = SPINLOCK_INIT;

static int serial_ready(void) {
    return inb(COM1 + 5) & 0x20;
}

void serial_init(void) {
    outb(COM1 + 1, 0x00); /* disable interrupts */
    outb(COM1 + 3, 0x80); /* enable divisor latch */
    outb(COM1 + 0, 0x03); /* 38400 baud (divisor 3) */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03); /* 8 bits, no parity, one stop */
    outb(COM1 + 2, 0xC7); /* FIFO on, 14-byte threshold */
    outb(COM1 + 4, 0x0B); /* RTS+DTR */
}

void serial_putc(char c) {
    irqflags_t flags = spin_lock_irqsave(&serial_lock);
    serial_putc_locked(c);
    spin_unlock_irqrestore(&serial_lock, flags);
}

irqflags_t serial_output_begin(void) {
    return spin_lock_irqsave(&serial_lock);
}

void serial_output_end(irqflags_t flags) {
    spin_unlock_irqrestore(&serial_lock, flags);
}

void serial_putc_locked(char c) {
    while (!serial_ready()) { /* spin */ }
    outb(COM1, (uint8_t)c);
}
