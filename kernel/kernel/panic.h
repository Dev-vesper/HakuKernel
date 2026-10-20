/* Haku panic handler: report the fatal condition, dump the crash frame,
 * then park the CPU with interrupts disabled. */
#ifndef HAKU_PANIC_H
#define HAKU_PANIC_H

#include "idt.h"

/* Fatal error without an interrupt frame (boot/setup failures). */
__declspec(noreturn) void panic(const char *message);
/* Fatal CPU exception: message plus the saved register frame. */
__declspec(noreturn) void panic_exception(const char *message,
                                          const regs_t *r);

#endif
