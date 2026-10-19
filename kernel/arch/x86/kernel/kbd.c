/* Haku PS/2 keyboard driver: IRQ1 + set-1 scancode translation.
 *
 * Set-1 scancodes are what the BIOS leaves the controller in after POST.
 * Bit 7 is set on "break" (key-up); everything else is "make" (key-down).
 * We track shift and caps-lock, translate through two lookup tables
 * (unshifted / shifted US layout), and push into a small ring buffer.
 */
#include "console.h"
#include "idt.h"
#include "kbd.h"
#include "pic.h"
#include "scheduler.h"
#include "sync.h"

#define KBD_DATA_PORT   0x60
#define KBD_STATUS_PORT 0x64

/* Ring buffer (power of two). 128 chars is plenty for interactive use. */
#define KBD_BUF_SIZE 128u
static volatile uint8_t kbd_buf[KBD_BUF_SIZE];
static volatile uint32_t kbd_head = 0;
static volatile uint32_t kbd_tail = 0;

/* Modifier state. */
static volatile int shift_down = 0;
static volatile int caps_lock = 0;

/* Console readers park here while the ring buffer is empty. The producer
 * (IRQ1) wakes them; a line-oriented caller simply re-parks after each
 * byte until it sees a newline. */
static wait_queue_t kbd_waiters = WAIT_QUEUE_INIT;

/* ------------------------------------------------------------------ */
/* Set-1 scancode -> ASCII tables (US layout, indices 0x00..0x3F).     */
/* 0 means "no ASCII translation".                                     */
/* ------------------------------------------------------------------ */
static const char scancode_map[0x40] = {
    0,    0x1B, '1',  '2',  '3',  '4',  '5',  '6',   /* 0x00-0x07 */
    '7',  '8',  '9',  '0',  '-',  '=',  0x08, '\t',  /* 0x08-0x0F */
    'q',  'w',  'e',  'r',  't',  'y',  'u',  'i',   /* 0x10-0x17 */
    'o',  'p',  '[',  ']',  '\n', 0,    'a',  's',   /* 0x18-0x1F */
    'd',  'f',  'g',  'h',  'j',  'k',  'l',  ';',   /* 0x20-0x27 */
    '\'', '`',  0,    '\\','z',  'x',  'c',  'v',   /* 0x28-0x2F */
    'b',  'n',  'm',  ',',  '.',  '/',  0,    '*',   /* 0x30-0x37 */
    0,    ' ',  0,    0,    0,    0,    0,    0,     /* 0x38-0x3F */
};

static const char scancode_map_shift[0x40] = {
    0,    0x1B, '!',  '@',  '#',  '$',  '%',  '^',   /* 0x00-0x07 */
    '&',  '*',  '(',  ')',  '_',  '+',  0x08, '\t',  /* 0x08-0x0F */
    'Q',  'W',  'E',  'R',  'T',  'Y',  'U',  'I',   /* 0x10-0x17 */
    'O',  'P',  '{',  '}',  '\n', 0,    'A',  'S',   /* 0x18-0x1F */
    'D',  'F',  'G',  'H',  'J',  'K',  'L',  ':',   /* 0x20-0x27 */
    '"',  '~',  0,    '|',  'Z',  'X',  'C',  'V',   /* 0x28-0x2F */
    'B',  'N',  'M',  '<',  '>',  '?',  0,    '*',   /* 0x30-0x37 */
    0,    ' ',  0,    0,    0,    0,    0,    0,     /* 0x38-0x3F */
};

/* ------------------------------------------------------------------ */
/* Ring buffer helpers (single-producer: IRQ1; single-consumer: task). */
/* ------------------------------------------------------------------ */
static void kbd_push(uint8_t ch) {
    uint32_t next = (kbd_head + 1) & (KBD_BUF_SIZE - 1);
    if (next == kbd_tail) return; /* buffer full, drop */
    kbd_buf[kbd_head] = ch;
    kbd_head = next;
}

int kbd_poll(void) {
    if (kbd_head == kbd_tail) return -1;
    uint8_t ch = kbd_buf[kbd_tail];
    kbd_tail = (kbd_tail + 1) & (KBD_BUF_SIZE - 1);
    return (int)ch;
}

int kbd_has_char(void) {
    return kbd_head != kbd_tail;
}

char kbd_getchar(void) {
    for (;;) {
        irqflags_t flags = irq_lock();
        int c = kbd_poll();
        if (c >= 0) {
            wait_queue_cancel(&kbd_waiters);
            irq_unlock(flags);
            return (char)c;
        }
        /* No byte queued: leave the run queue instead of spinning. IRQs
         * are still masked here, so no key can arrive between the empty
         * poll and the sleep; sti's interrupt shadow then makes the
         * sti/hlt pair atomic, so either IRQ1 wakes us immediately or the
         * timer parks the task and runs another one. */
        wait_queue_sleep(&kbd_waiters);
        __asm {
            sti
            hlt
        }
    }
}

/* ------------------------------------------------------------------ */
/* IRQ1 handler.                                                       */
/* ------------------------------------------------------------------ */
static void kbd_irq(regs_t *r) {
    (void)r;
    uint8_t sc = inb(KBD_DATA_PORT);

    if (sc == 0xE0) return; /* extended prefix — ignore for now */

    int break_code = (sc & 0x80) != 0;
    uint8_t idx = sc & 0x7F;

    /* Modifier keys. */
    switch (idx) {
    case 0x2A: /* left shift  */
    case 0x36: /* right shift */
        shift_down = !break_code;
        return;
    case 0x3A: /* caps lock (toggle on make only) */
        if (!break_code) caps_lock = !caps_lock;
        return;
    }

    if (break_code) return; /* ignore all other key-ups */

    /* Translate to ASCII. */
    char ch;
    if (shift_down ^ caps_lock)
        ch = scancode_map_shift[idx];
    else
        ch = scancode_map[idx];

    if (ch) {
        kbd_push((uint8_t)ch);
        /* Wake console readers parked on the input wait queue. */
        wait_queue_wake_all(&kbd_waiters);
    }
}

/* ------------------------------------------------------------------ */
/* Init.                                                               */
/* ------------------------------------------------------------------ */
void kbd_init(void) {
    irq_install(1, kbd_irq);
    pic_clear_mask(1); /* unmask IRQ1 on the master PIC */
}
