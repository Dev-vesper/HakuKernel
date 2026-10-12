/* Haku PS/2 keyboard driver (IRQ1).
 *
 * Follows the Linux early PS/2 keyboard path (drivers/input/keyboard/atkbd.c
 * simplified): read scancode from port 0x60 on IRQ1, translate set-1 scancodes
 * to ASCII with shift/caps-lock state, push into a ring buffer.
 *
 * kbd_getchar() blocks; kbd_poll() is non-blocking. The driver never
 * allocates — it is safe to call before the heap.
 */
#ifndef HAKU_KBD_H
#define HAKU_KBD_H

#include "cpu.h"

/* Public API. */
void kbd_init(void);            /* install IRQ1 handler, enable IRQ1 on the PIC */
int kbd_poll(void);             /* -1 if empty, else ASCII char (0 for non-ASCII) */
char kbd_getchar(void);         /* block until a character is available */
int kbd_has_char(void);         /* 1 if a char is waiting */

#endif
