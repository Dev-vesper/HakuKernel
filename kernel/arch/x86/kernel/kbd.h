/* Haku PS/2 keyboard driver (IRQ1).
 *
 * Follows the Linux early PS/2 keyboard path (drivers/input/keyboard/atkbd.c
 * simplified): read scancode from port 0x60 on IRQ1, translate set-1 scancodes
 * to ASCII with shift/caps-lock state, and forward the character to the
 * terminal input queue. The driver never allocates, blocks, or knows about
 * tasks — terminal_getchar() in drivers/terminal.c owns that policy.
 */
#ifndef HAKU_KBD_H
#define HAKU_KBD_H

#include "cpu.h"

/* Install the IRQ1 handler and enable IRQ1 on the PIC. */
void kbd_init(void);

#endif
