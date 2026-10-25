/* Haku kernel synchronization primitives.
 *
 * Locking rules (see README "Locking" for the full table):
 *  - irq_lock/irq_unlock: short critical sections that must not be
 *    interrupted and that never call into a blocking path. Nests.
 *  - mutex_lock/mutex_unlock: mutual exclusion for task-context critical
 *    sections that may be preempted and that may be held across I/O (VFS,
 *    disk). Interrupts stay enabled, and a waiter parks on a wait queue
 *    instead of burning the CPU. Never acquire one with interrupts
 *    disabled, never block inside the same task twice.
 *  - spin_lock/spin_unlock: mutual exclusion for very short task-context
 *    sections. Waiters busy-wait, so nothing that can take a long time may
 *    happen while one is held.
 *  - spin_lock_irqsave/spin_unlock_irqrestore: mutual exclusion for state
 *    reachable from both task and interrupt context (allocators, console).
 *    The holder cannot be preempted, so the lock is never observed held by
 *    a stopped task.
 */
#ifndef HAKU_SYNC_H
#define HAKU_SYNC_H

#include "cpu.h"
#include "scheduler.h"

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

/* Sleeping mutual exclusion for task context. A waiter parks on the lock's
 * wait queue and is woken by the unlock, so a task that is waiting for the
 * disk or the filesystem costs no CPU. Boot and idle context (no task to
 * park) falls back to a bounded spin, which is safe because those paths are
 * single-threaded. Not recursive: a task that locks a mutex it already
 * holds waits forever. */
typedef struct {
    volatile uint32_t locked;
    volatile uint32_t owner;    /* pid of the holder, 0 for boot context */
    wait_queue_t waiters;
} mutex_t;

#define MUTEX_INIT { 0u, 0u, WAIT_QUEUE_INIT }

void mutex_init(mutex_t *mutex);
/* Acquire, blocking the current task until the lock is free. */
void mutex_lock(mutex_t *mutex);
/* Acquire without blocking; returns 1 on success. */
int mutex_trylock(mutex_t *mutex);
/* Release and wake every waiter; each re-tests the lock. */
void mutex_unlock(mutex_t *mutex);
/* 1 when the calling task holds the mutex (used by assertions and tests). */
int mutex_held_by_current(const mutex_t *mutex);

#endif
