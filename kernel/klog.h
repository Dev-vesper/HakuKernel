/* Haku klog: Linux-dmesg-style boot log.
 *
 * Every line opens with "[    S.UUUUUU] " where the stamp is seconds since
 * boot from the 100 Hz PIT (0.000000 until pit_init runs, exactly like
 * early Linux messages before the clocksource is up). All output goes to
 * both VGA and COM1, so the screen and the headless serial log agree.
 *
 * The headless markers (HAKU-BOOT-OK, IDT-OK, ...) live inside these lines
 * as subsystem tags; build.py greps for them as substrings.
 */
#ifndef HAKU_KLOG_H
#define HAKU_KLOG_H

#include "cpu.h"

void klog_prefix(void);                      /* "[    0.000000] " */
void klog_putc(char c);                      /* VGA + serial */
void klog_write(const char *s);              /* VGA + serial */
void klog_dec(uint32_t v);                   /* decimal, both */
void klog_hex(uint32_t v);                   /* 0x + 8 digits, both */
void klog_hex_raw(uint32_t v);               /* 8 digits, no prefix, both */

#endif
