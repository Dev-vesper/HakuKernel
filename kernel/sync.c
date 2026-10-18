#include "sync.h"

irqflags_t irq_lock(void) {
    return cpu_irq_save();
}

void irq_unlock(irqflags_t flags) {
    cpu_irq_restore(flags);
}

void spin_init(spinlock_t *lock) {
    lock->locked = 0;
}

/* Test-and-set under a brief interrupt mask so the acquisition is atomic
 * with respect to the timer. After that the critical section runs with
 * interrupts enabled: if the holder is preempted, the timer will schedule
 * it again while this task busy-waits. Callers must have interrupts
 * enabled (boot and the quiescent idle self-test are single-threaded). */
void spin_lock(spinlock_t *lock) {
    for (;;) {
        irqflags_t flags = irq_lock();
        if (!lock->locked) {
            lock->locked = 1;
            irq_unlock(flags);
            return;
        }
        irq_unlock(flags);
        while (lock->locked) {
            /*
             * The condition is volatile, so each iteration re-reads the
             * word. Yielding to the timer here is what lets the holder
             * make progress; a dedicated wait queue is a later milestone.
             */
        }
    }
}

void spin_unlock(spinlock_t *lock) {
    irqflags_t flags = irq_lock();
    lock->locked = 0;
    irq_unlock(flags);
}

/* All holders keep interrupts disabled, so no other task can be stopped
 * while holding one of these locks; the busy-wait only runs if the same
 * context re-acquires the lock, which the rules forbid. */
irqflags_t spin_lock_irqsave(spinlock_t *lock) {
    irqflags_t flags = irq_lock();
    while (lock->locked) {
    }
    lock->locked = 1;
    return flags;
}

void spin_unlock_irqrestore(spinlock_t *lock, irqflags_t flags) {
    lock->locked = 0;
    irq_unlock(flags);
}
