/* Haku framebuffer: a text console drawn into a VBE linear framebuffer.
 *
 * The bootloader discovers the mode (INT 10h AX=4F00h/4F01h/4F02h) and
 * leaves a handover record behind; this driver maps the aperture, copies
 * the 8x16 VGA font and renders the console. When no usable linear
 * framebuffer is found the driver stays inactive and the console keeps
 * using the VGA text buffer.
 */
#ifndef HAKU_FRAMEBUFFER_H
#define HAKU_FRAMEBUFFER_H

#include "cpu.h"
#include "display.h"

/* Handover record left by stage 2 at physical 0x6FC0 (little-endian). */
#define FB_HANDOVER_PHYS   0x6FC0u
#define FB_HANDOVER_MAGIC  0x31424648u   /* 'HFB1' */

/* The 8x16 VGA font copied out of the video BIOS ROM by stage 2. */
#define FB_FONT_PHYS       0x9000u
#define FB_FONT_BYTES      4096u
#define FB_CHAR_WIDTH      8u
#define FB_CHAR_HEIGHT     16u

/* Map the aperture, install the font and clear the screen. Returns 1 when
 * the framebuffer console is live, 0 when the VGA text fallback is used. */
int framebuffer_init(void);
int framebuffer_active(void);
/* Write one character at the console cursor (handles '\n'). */
void framebuffer_putc(char c);
/* Draw a string in the status bar at the top-right (used by the timer). */
void framebuffer_status(const char *text);
uint32_t framebuffer_width(void);
uint32_t framebuffer_height(void);

/* display.h backend table; selected once framebuffer_init() succeeds. */
extern const display_ops_t framebuffer_display;

#endif
