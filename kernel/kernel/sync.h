/* Haku kernel synchronization primitives.
 *
 * Locking rules (see README "Locking" for the full table):
 *  - irq_lock/irq_unlock: short critical sections that must not be
 *    interrupted and that never call into a blocking path. Nests.
 *  - spin_lock/spin_unlock: mutual exclusion for task-context critical
 *    sections that may be preempted (VFS, disk I/O). The section runs with
 *    interrupts enabled, so a preempted holder can be scheduled again while
 *    waiters busy-wait. Never acquire one with interrupts disabled, never
 *    sleep or block while holding one.
 *  - spin_lock_irqsave/spin_unlock_irqrestore: mutual exclusion for state
 *    reachable from both task and interrupt context (allocators, console).
 *    The holder cannot be preempted, so the lock is never observed held by
 *    a stopped task.
 */
#ifndef HAKU_SYNC_H
#define HAKU_SYNC_H

#include "cpu.h"

typedef uint32_t irqflags_t;

/* Disable maskable interrupts, returning the previous EFLAGS.IF state. */
irqflags_t irq_lock(void);
void irq_unlock(irqflags_t flags);

typedef struct {
    volatile uint32_t locked;
} spinlock_t;

#define SPINLOCK_INIT { 0u }

void spin_init(spinlock_t *lock);

/* Task-context spinlock: interrupts stay enabled for the critical section. */
void spin_lock(spinlock_t *lock);
void spin_unlock(spinlock_t *lock);

/* Interrupt-safe spinlock: interrupts stay disabled until the matching
 * irqrestore, which also makes the critical section non-preemptible. */
irqflags_t spin_lock_irqsave(spinlock_t *lock);
void spin_unlock_irqrestore(spinlock_t *lock, irqflags_t flags);

#endif
