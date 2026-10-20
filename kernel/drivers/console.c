/* Haku console: VGA text mode + COM1 serial output. When a linear
 * framebuffer is available the text console is drawn there instead; the
 * VGA text buffer stays the fallback for machines without one. */
#include "console.h"
#include "framebuffer.h"
#include "sync.h"

static volatile uint16_t *const VGA = (uint16_t *)0xB8000;
static uint8_t vga_row = 0;
static uint8_t vga_col = 0;
static uint8_t vga_color = 0x0F; /* bright white on black */
/* The console may be written from both task and interrupt context (klog),
 * so cursor/UART state uses IRQ-save locks: a holder is never preempted,
 * which makes the short critical sections safe from every context. */
static spinlock_t vga_lock = SPINLOCK_INIT;
static spinlock_t serial_lock = SPINLOCK_INIT;

void vga_clear(uint8_t color) {
    uint16_t blank = (uint16_t)((color << 8) | ' ');
    irqflags_t flags = spin_lock_irqsave(&vga_lock);
    for (int i = 0; i < VGA_COLS * VGA_ROWS; i++)
        VGA[i] = blank;
    vga_row = 0;
    vga_col = 0;
    vga_color = color;
    spin_unlock_irqrestore(&vga_lock, flags);
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
    irqflags_t flags = spin_lock_irqsave(&vga_lock);
    if (c == '\n') {
        vga_col = 0;
        if (++vga_row >= VGA_ROWS) vga_scroll();
        spin_unlock_irqrestore(&vga_lock, flags);
        return;
    }
    VGA[vga_row * VGA_COLS + vga_col] =
        (uint16_t)((vga_color << 8) | (uint8_t)c);
    if (++vga_col >= VGA_COLS) {
        vga_col = 0;
        if (++vga_row >= VGA_ROWS) vga_scroll();
    }
    spin_unlock_irqrestore(&vga_lock, flags);
}

void console_putc(char c) {
    if (framebuffer_active()) {
        framebuffer_putc(c);
        return;
    }
    vga_putc(c);
}

/* These three are the console's display primitives: klog and the exception
 * paths emit through them, so they must follow console_putc() and reach the
 * framebuffer when one is live instead of writing to the invisible VGA text
 * buffer in graphics mode. */
void vga_write(const char *s) {
    while (*s) console_putc(*s++);
}

static void put_hex(void (*emit)(char), uint32_t v) {
    static const char digits[] = "0123456789ABCDEF";
    emit('0');
    emit('x');
    for (int shift = 28; shift >= 0; shift -= 4)
        emit(digits[(v >> shift) & 0xF]);
}

void vga_hex(uint32_t v) {
    put_hex(console_putc, v);
}

static void put_dec(void (*emit)(char), uint32_t v) {
    char rev[10];
    int n = 0;
    do {
        rev[n++] = (char)('0' + (v % 10));
        v /= 10;
    } while (v && n < 10);
    while (n--) emit(rev[n]);
}

void vga_dec(uint32_t v) {
    put_dec(console_putc, v);
}

/* Draw a short status string at the top-right corner (overwrites in place). */
void vga_corner(const char *s) {
    size_t len = strlen(s);
    irqflags_t flags;
    if (framebuffer_active()) {
        framebuffer_status(s);
        return;
    }
    if (len > VGA_COLS) len = VGA_COLS;
    flags = spin_lock_irqsave(&vga_lock);
    for (size_t i = 0; i < len; i++)
        VGA[(VGA_COLS - len) + i] =
            (uint16_t)(((uint16_t)0x0E << 8) | (uint8_t)s[i]); /* yellow */
    spin_unlock_irqrestore(&vga_lock, flags);
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
    irqflags_t flags = spin_lock_irqsave(&serial_lock);
    while (!serial_ready()) { /* spin */ }
    outb(COM1, (uint8_t)c);
    spin_unlock_irqrestore(&serial_lock, flags);
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

void serial_dec(uint32_t v) {
    put_dec(serial_putc, v);
}

void console_init(void) {
    vga_clear(0x1F); /* white on blue, classic */
    serial_init();
}
