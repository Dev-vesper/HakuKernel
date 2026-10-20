/* Haku klog implementation: serial-mirrored terminal emit with PIT
 * timestamps. The terminal sends each byte to the active display and COM1,
 * so there is a single output path. */
#include "klog.h"
#include "pit.h"
#include "terminal.h"

static klog_level_t log_threshold = KLOG_INFO;

static const char *const level_names[] = {
    "DEBUG", "INFO", "WARN", "ERROR", "CRIT"
};

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

/* --- leveled logging --------------------------------------------------- */

void klog_set_level(klog_level_t level) {
    log_threshold = level;
}

klog_level_t klog_get_level(void) {
    return log_threshold;
}

const char *klog_level_name(klog_level_t level) {
    if (level < KLOG_DEBUG || level > KLOG_CRIT) return "NONE";
    return level_names[level];
}

int klog_level_enabled(klog_level_t level) {
    return level >= log_threshold && level <= KLOG_CRIT;
}

int klog_begin(klog_level_t level) {
    if (!klog_level_enabled(level)) return 0;
    klog_prefix();
    klog_write(klog_level_name(level));
    klog_write(": ");
    return 1;
}

void klog_log(klog_level_t level, const char *message) {
    if (klog_begin(level)) klog_write(message);
}

void klog_self_test(void) {
    int hidden = klog_begin(KLOG_DEBUG); /* INFO threshold: must be 0 */
    int shown;

    klog_set_level(KLOG_DEBUG);
    shown = klog_begin(KLOG_DEBUG);
    if (shown) klog_write("KLOG-LEVELS-OK: debug output gated by level\n");
    klog_set_level(KLOG_INFO);

    if (hidden || !shown) {
        klog_log(KLOG_ERROR, "KLOG-LEVELS-FAIL: level filtering broken\n");
    }
}
