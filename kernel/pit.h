/* Haku PIT driver: Intel 8253/8254 channel 0 as the system timer.
 * Programs mode 3 (square wave) at the requested Hz and counts ticks;
 * each full second is reported on serial (headless proof) and drawn in
 * the VGA corner (visible proof). */
#ifndef HAKU_PIT_H
#define HAKU_PIT_H

#include "cpu.h"

void pit_init(uint32_t hz);
uint32_t pit_ticks(void);
uint32_t pit_seconds(void);

#endif
