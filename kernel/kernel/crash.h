/* Haku crash reports: a uniform register/MMU dump for fatal kernel faults
 * and for contained ring-3 faults. Output goes through klog, so it reaches
 * both the display and the QEMU serial log. */
#ifndef HAKU_CRASH_H
#define HAKU_CRASH_H

#include "idt.h"

/* Print control registers, process id, the exception frame, general
 * registers, and (when safely mapped) the top of the interrupted stack.
 * r may be 0 for panics outside an interrupt frame. */
void crash_report(const char *tag, const regs_t *r);

#endif
