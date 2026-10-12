/* Haku PIT driver: 100 Hz-class system timer on IRQ0. */
#include "console.h"
#include "idt.h"
#include "pic.h"
#include "pit.h"

#define PIT_CH0 0x40
#define PIT_CMD 0x43
#define PIT_INPUT_HZ 1193180

static volatile uint32_t ticks = 0;
static uint32_t hz = 100;
static uint32_t seconds = 0;

static void pit_tick(regs_t *r) {
    (void)r;
    ticks++;
    if (ticks % hz == 0) {
        seconds++;
        serial_write("T+");
        serial_dec(seconds);
        serial_write("s\n");
        /* Heartbeat in the top-right corner (redrawn every second). */
        char buf[16];
        uint32_t v = seconds;
        int len = 0;
        buf[len++] = 'T';
        buf[len++] = '+';
        char rev[10];
        int n = 0;
        do {
            rev[n++] = (char)('0' + (v % 10));
            v /= 10;
        } while (v && n < 10);
        while (n--) buf[len++] = rev[n];
        buf[len++] = 's';
        buf[len] = 0;
        vga_corner(buf);
    }
}

void pit_init(uint32_t freq) {
    hz = freq ? freq : 100;
    uint32_t div = PIT_INPUT_HZ / hz;
    outb(PIT_CMD, 0x36);                  /* ch0, lo/hi access, mode 3 */
    outb(PIT_CH0, (uint8_t)(div & 0xFF));
    outb(PIT_CH0, (uint8_t)((div >> 8) & 0xFF));
    irq_install(0, pit_tick);
}

uint32_t pit_ticks(void) {
    return ticks;
}

uint32_t pit_seconds(void) {
    return seconds;
}
