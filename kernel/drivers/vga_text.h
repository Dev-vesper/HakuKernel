/* Haku VGA text backend: the 80x25 buffer at 0xB8000. */
#ifndef HAKU_VGA_TEXT_H
#define HAKU_VGA_TEXT_H

#include "display.h"

#define VGA_COLS 80
#define VGA_ROWS 25

extern const display_ops_t vga_text_display;

#endif
