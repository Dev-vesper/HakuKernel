/* Haku display backends: the shared text-output API.
 *
 * A backend owns one screen. The VGA text buffer is always available and is
 * selected first; the framebuffer driver registers itself as the active
 * backend once it has mapped a mode. The terminal talks only to this
 * interface, so adding another display does not touch input or syscalls.
 */
#ifndef HAKU_DISPLAY_H
#define HAKU_DISPLAY_H

#include "cpu.h"

typedef struct display_ops {
    const char *name;
    void (*clear)(uint8_t color);
    void (*putc)(char c);
    void (*status)(const char *text); /* top-right status line */
} display_ops_t;

/* Select the VGA text backend and clear the screen. */
void display_init(void);
/* Make a backend current (called after its probe succeeded). */
void display_use(const display_ops_t *ops);
const display_ops_t *display_active(void);
const char *display_name(void);

void display_putc(char c);
void display_clear(uint8_t color);
void display_status(const char *text);

#endif
