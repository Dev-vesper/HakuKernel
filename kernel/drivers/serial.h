/* Haku COM1 serial backend (polled, headless telemetry). */
#ifndef HAKU_SERIAL_H
#define HAKU_SERIAL_H

#include "cpu.h"
#include "sync.h"

void serial_init(void);
void serial_putc(char c);

/* Hold the output lock across a group of bytes. The lock is IRQ-save, so
 * interrupts stay off until serial_output_end(); keep the group short
 * (the UART runs at 38400 baud, so 256 bytes take about 70 ms). Holding
 * it for a whole write keeps a kernel log line and a user write from
 * interleaving, which is what keeps the test markers in the serial log
 * contiguous. */
irqflags_t serial_output_begin(void);
void serial_output_end(irqflags_t flags);
void serial_putc_locked(char c);

#endif
