/* Haku terminal implementation. */
#include "terminal.h"
#include "display.h"
#include "scheduler.h"
#include "serial.h"
#include "sync.h"

/* Single-producer (IRQ1) / single-consumer (task) byte ring. */
static volatile uint8_t input_buf[TERMINAL_BUF_SIZE];
static volatile uint32_t input_head = 0;
static volatile uint32_t input_tail = 0;

/* Tasks park here while the ring is empty. The keyboard IRQ wakes them. */
static wait_queue_t input_waiters = WAIT_QUEUE_INIT;

/* --- output ----------------------------------------------------------- */

void terminal_putc(char c) {
    display_putc(c);
    if (c == '\n') serial_putc('\r'); /* serial is CRLF */
    serial_putc(c);
}

void terminal_write(const char *s) {
    while (*s) terminal_putc(*s++);
}

static void put_hex(uint32_t v) {
    static const char digits[] = "0123456789ABCDEF";
    terminal_putc('0');
    terminal_putc('x');
    for (int shift = 28; shift >= 0; shift -= 4)
        terminal_putc(digits[(v >> shift) & 0xF]);
}

void terminal_hex(uint32_t v) {
    put_hex(v);
}

void terminal_dec(uint32_t v) {
    char rev[10];
    int n = 0;
    do {
        rev[n++] = (char)('0' + (v % 10));
        v /= 10;
    } while (v && n < 10);
    while (n--) terminal_putc(rev[n]);
}

void terminal_corner(const char *s) {
    display_status(s);
}

int32_t terminal_write_bytes(const void *buffer, uint32_t count,
                             uint32_t *out_count) {
    const uint8_t *bytes = (const uint8_t *)buffer;
    for (uint32_t i = 0; i < count; i++)
        terminal_putc((char)bytes[i]);
    *out_count = count;
    return 1;
}

/* --- input ------------------------------------------------------------ */

void terminal_input_push(char c) {
    uint32_t next = (input_head + 1u) & (TERMINAL_BUF_SIZE - 1u);
    if (next == input_tail) return; /* full, drop */
    input_buf[input_head] = (uint8_t)c;
    input_head = next;
    wait_queue_wake_all(&input_waiters);
}

int terminal_input_poll(void) {
    if (input_head == input_tail) return -1;
    uint8_t ch = input_buf[input_tail];
    input_tail = (input_tail + 1u) & (TERMINAL_BUF_SIZE - 1u);
    return (int)ch;
}

char terminal_getchar(void) {
    for (;;) {
        irqflags_t flags = irq_lock();
        int c = terminal_input_poll();
        if (c >= 0) {
            wait_queue_cancel(&input_waiters);
            irq_unlock(flags);
            return (char)c;
        }
        /* No byte queued: leave the run queue instead of spinning. IRQs
         * are still masked here, so no key can arrive between the empty
         * poll and the sleep; sti's interrupt shadow then makes the
         * sti/hlt pair atomic, so either IRQ1 wakes us immediately or the
         * timer parks the task and runs another one. */
        wait_queue_sleep(&input_waiters);
        __asm {
            sti
            hlt
        }
    }
}

int32_t terminal_read(void *buffer, uint32_t count, uint32_t *out_count) {
    uint8_t *bytes = (uint8_t *)buffer;
    uint32_t copied = 0;
    while (copied < count) {
        char ch = terminal_getchar();
        bytes[copied++] = (uint8_t)ch;
        if (ch == '\n') break;
    }
    *out_count = copied;
    return 1;
}

/* --- init ------------------------------------------------------------- */

void terminal_init(void) {
    serial_init();
    display_init();
}
