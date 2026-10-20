/* Haku COM1 serial backend (polled, headless telemetry). */
#ifndef HAKU_SERIAL_H
#define HAKU_SERIAL_H

#include "cpu.h"

void serial_init(void);
void serial_putc(char c);

#endif
