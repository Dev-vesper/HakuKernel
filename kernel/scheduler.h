#ifndef HAKU_SCHEDULER_H
#define HAKU_SCHEDULER_H

#include "elf.h"
#include "idt.h"

#define SCHEDULER_MAX_PROCESSES 4u

int scheduler_init(loaded_elf_t *initial_program);
int scheduler_user_tasks_done(void);
void scheduler_start(void);
regs_t *scheduler_preempt(regs_t *frame);
regs_t *scheduler_exit_current(regs_t *frame);
int scheduler_exec_current(loaded_elf_t *replacement, regs_t *frame);
int32_t scheduler_spawn(loaded_elf_t *program);
regs_t *scheduler_waitpid(uint32_t pid, uint32_t user_status,
                          uint32_t options, regs_t *frame);
regs_t *scheduler_sleep_ticks(uint32_t ticks, regs_t *frame);
uint32_t scheduler_current_pid(void);

#endif
