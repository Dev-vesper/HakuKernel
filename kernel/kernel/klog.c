/* Haku klog implementation: serial-mirrored terminal emit with PIT
 * timestamps. The terminal sends each byte to the active display and COM1,
 * so there is a single output path. */
#include "klog.h"
#include "pit.h"
#include "terminal.h"

void klog_putc(char c) {
    terminal_putc(c);
}

void klog_write(const char *s) {
    terminal_write(s);
}

void klog_dec(uint32_t v) {
    terminal_dec(v);
}

void klog_hex(uint32_t v) {
    terminal_hex(v);
}

void klog_hex_raw(uint32_t v) {
    for (int shift = 28; shift >= 0; shift -= 4) {
        static const char digits[] = "0123456789ABCDEF";
        terminal_putc(digits[(v >> shift) & 0xF]);
    }
}

static void emit_padded(uint32_t v, int width, char pad) {
    char rev[10];
    int n = 0;
    do {
        rev[n++] = (char)('0' + (v % 10));
        v /= 10;
    } while (v && n < 10);
    for (int i = n; i < width; i++)
        terminal_putc(pad);
    while (n--) terminal_putc(rev[n]);
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
