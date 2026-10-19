#ifndef HAKU_SCHEDULER_H
#define HAKU_SCHEDULER_H

#include "elf.h"
#include "haku/abi.h"
#include "idt.h"

#define SCHEDULER_MAX_PROCESSES 4u

/* Event wait queue. A task that cannot make progress parks itself on a
 * queue and is woken by whoever satisfies the event (an IRQ handler or
 * another task); a parked task is off the run queue and does not spin.
 * The queue is a bitmap over the scheduler's task slots, so it needs no
 * memory and cannot fail. sleep/cancel must be called with interrupts
 * disabled (the scheduler's task-state rule); wake is IRQ-safe. */
typedef struct {
    volatile uint32_t waiters;
} wait_queue_t;

#define WAIT_QUEUE_INIT { 0u }

/* Mark the current task blocked on the queue. The caller must immediately
 * execute sti;hlt so either the waking IRQ returns straight back or the
 * timer IRQ switches away. Returns 1 when a task was parked. */
int wait_queue_sleep(wait_queue_t *queue);
/* Leave the queue and make the current task runnable again. */
void wait_queue_cancel(wait_queue_t *queue);
/* Wake every parked task; each re-checks its condition after resuming.
 * The bitmap entry is dropped even for tasks that are no longer blocked. */
int wait_queue_wake_all(wait_queue_t *queue);

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
uint32_t scheduler_brk(uint32_t requested);
uint32_t scheduler_mmap(uint32_t length);
uint32_t scheduler_munmap(uint32_t address, uint32_t length);
uint32_t scheduler_current_pid(void);
void scheduler_note_syscall(void);
int scheduler_get_usage(haku_usage_t *usage);

#endif
