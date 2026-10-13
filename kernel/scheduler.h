#ifndef HAKU_SCHEDULER_H
#define HAKU_SCHEDULER_H

#include "elf.h"
#include "idt.h"

#define SCHEDULER_MAX_PROCESSES 4u

int scheduler_init(loaded_elf_t *programs, uint32_t count);
int scheduler_user_tasks_done(void);
void scheduler_start(void);
regs_t *scheduler_preempt(regs_t *frame);
regs_t *scheduler_exit_current(regs_t *frame);
uint32_t scheduler_current_pid(void);

#endif
