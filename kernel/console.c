/* MyOS console: VGA text mode + COM1 serial output. */
#include "console.h"

static volatile uint16_t *const VGA = (uint16_t *)0xB8000;
static uint8_t vga_row = 0;
static uint8_t vga_col = 0;
static uint8_t vga_color = 0x0F; /* bright white on black */

void vga_clear(uint8_t color) {
    uint16_t blank = (uint16_t)((color << 8) | ' ');
    for (int i = 0; i < VGA_COLS * VGA_ROWS; i++)
        VGA[i] = blank;
    vga_row = 0;
    vga_col = 0;
    vga_color = color;
}

static void vga_scroll(void) {
    memmove((void *)VGA, (void *)(VGA + VGA_COLS),
            (VGA_ROWS - 1) * VGA_COLS * sizeof(uint16_t));
    uint16_t blank = (uint16_t)((vga_color << 8) | ' ');
    for (int x = 0; x < VGA_COLS; x++)
        VGA[(VGA_ROWS - 1) * VGA_COLS + x] = blank;
    vga_row = VGA_ROWS - 1;
}

static void vga_putc(char c) {
    if (c == '\n') {
        vga_col = 0;
        if (++vga_row >= VGA_ROWS) vga_scroll();
        return;
    }
    VGA[vga_row * VGA_COLS + vga_col] =
        (uint16_t)((vga_color << 8) | (uint8_t)c);
    if (++vga_col >= VGA_COLS) {
        vga_col = 0;
        if (++vga_row >= VGA_ROWS) vga_scroll();
    }
}

void vga_write(const char *s) {
    while (*s) vga_putc(*s++);
}

static void put_hex(void (*emit)(char), uint32_t v) {
    static const char digits[] = "0123456789ABCDEF";
    emit('0');
    emit('x');
    for (int shift = 28; shift >= 0; shift -= 4)
        emit(digits[(v >> shift) & 0xF]);
}

void vga_hex(uint32_t v) {
    put_hex(vga_putc, v);
}

#define COM1 0x3F8

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
    while (!serial_ready()) { /* spin */ }
    outb(COM1, (uint8_t)c);
}

void serial_write(const char *s) {
    while (*s) {
        if (*s == '\n') serial_putc('\r');
        serial_putc(*s++);
    }
}

void serial_hex(uint32_t v) {
    put_hex(serial_putc, v);
}

void console_init(void) {
    vga_clear(0x1F); /* white on blue, classic */
    serial_init();
}
