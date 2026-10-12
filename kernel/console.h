/* Haku console: VGA text mode + COM1 serial output. Safe to call from
 * interrupt handlers (no locks needed yet: single CPU, handlers don't nest
 * because IDT gates are interrupt gates). */
#ifndef HAKU_CONSOLE_H
#define HAKU_CONSOLE_H

#include "cpu.h"

#define VGA_COLS 80
#define VGA_ROWS 25

void console_init(void);
void vga_clear(uint8_t color);
void vga_write(const char *s);
void vga_hex(uint32_t v);
void vga_dec(uint32_t v);
void vga_corner(const char *s);
void serial_init(void);
void serial_putc(char c);
void serial_write(const char *s);
void serial_hex(uint32_t v);
void serial_dec(uint32_t v);

#endif
