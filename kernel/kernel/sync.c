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

void mutex_init(mutex_t *mutex) {
    mutex->locked = 0;
    mutex->owner = 0;
    mutex->waiters.waiters = 0;
}

/* Park the current task on the mutex until it can take the lock. The
 * decision and the park happen in one IRQ-off section, so an unlock cannot
 * slip in between the test and the sleep and leave the waiter parked with
 * nobody left to wake it. The park re-enables interrupts before halting:
 * the unlock (or the timer, which simply re-selects us) resumes execution
 * right here. */
void mutex_lock(mutex_t *mutex) {
    for (;;) {
        irqflags_t flags = irq_lock();
        if (!mutex->locked) {
            mutex->locked = 1;
            mutex->owner = scheduler_current_pid();
            irq_unlock(flags);
            return;
        }
        if (wait_queue_sleep(&mutex->waiters)) {
            __asm {
                sti
                hlt
            }
            continue;
        }
        /* Boot or idle context: there is no task to switch to, so the
         * holder can only be this context, which the rules forbid. Spin
         * with interrupts on so the timer can still make progress. */
        irq_unlock(flags);
        while (mutex->locked) {
        }
    }
}

int mutex_trylock(mutex_t *mutex) {
    irqflags_t flags = irq_lock();
    if (mutex->locked) {
        irq_unlock(flags);
        return 0;
    }
    mutex->locked = 1;
    mutex->owner = scheduler_current_pid();
    irq_unlock(flags);
    return 1;
}

void mutex_unlock(mutex_t *mutex) {
    irqflags_t flags = irq_lock();
    mutex->locked = 0;
    mutex->owner = 0;
    /* Every waiter re-tests the lock, so waking all of them costs at most
     * one extra pass through the loop per waiter and keeps the fast path
     * free of a waiter count. */
    wait_queue_wake_all(&mutex->waiters);
    irq_unlock(flags);
}

int mutex_held_by_current(const mutex_t *mutex) {
    uint32_t pid = scheduler_current_pid();
    return mutex->locked && mutex->owner == pid;
}
