/* Haku klog implementation: dual VGA+serial emit with PIT timestamps. */
#include "klog.h"
#include "console.h"
#include "pit.h"

void klog_putc(char c) {
    console_putc(c);
    serial_putc(c);
}

void klog_write(const char *s) {
    while (*s) {
        if (*s == '\n') {
            console_putc('\n');
            serial_putc('\r');
            serial_putc('\n');
        } else {
            console_putc(*s);
            serial_putc(*s);
        }
        s++;
    }
}

void klog_dec(uint32_t v) {
    vga_dec(v);
    serial_dec(v);
}

void klog_hex(uint32_t v) {
    vga_hex(v);
    serial_hex(v);
}

void klog_hex_raw(uint32_t v) {
    /* console has no raw-hex primitive, so spell it via putc pair. */
    for (int shift = 28; shift >= 0; shift -= 4) {
        static const char digits[] = "0123456789ABCDEF";
        char c = digits[(v >> shift) & 0xF];
        console_putc(c);
        serial_putc(c);
    }
}

static void emit_padded(uint32_t v, int width, char pad) {
    char rev[10];
    int n = 0;
    do {
        rev[n++] = (char)('0' + (v % 10));
        v /= 10;
    } while (v && n < 10);
    for (int i = n; i < width; i++) {
        console_putc(pad);
        serial_putc(pad);
    }
    while (n--) {
        console_putc(rev[n]);
        serial_putc(rev[n]);
    }
}

void klog_prefix(void) {
    /* Linux "[%5u.%06u] ": space-padded seconds, zero-padded micros. */
    uint32_t ticks = pit_ticks();
    klog_write("[");
    emit_padded(ticks / 100u, 5, ' ');
    klog_write(".");
    emit_padded((ticks % 100u) * 10000u, 6, '0');
    klog_write("] ");
}
