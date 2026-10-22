#ifndef HAKU_FUTEX_H
#define HAKU_FUTEX_H

#include <haku/types.h>
#include <haku/abi.h>

/* futex(2) operations. The command is the operation without the flags. */
#define FUTEX_WAIT           HAKU_FUTEX_WAIT
#define FUTEX_WAKE           HAKU_FUTEX_WAKE
#define FUTEX_REQUEUE        HAKU_FUTEX_REQUEUE
#define FUTEX_CMP_REQUEUE    HAKU_FUTEX_CMP_REQUEUE
#define FUTEX_WAKE_OP        HAKU_FUTEX_WAKE_OP
#define FUTEX_LOCK_PI        HAKU_FUTEX_LOCK_PI
#define FUTEX_UNLOCK_PI      HAKU_FUTEX_UNLOCK_PI
#define FUTEX_TRYLOCK_PI     HAKU_FUTEX_TRYLOCK_PI
#define FUTEX_WAIT_BITSET    HAKU_FUTEX_WAIT_BITSET
#define FUTEX_WAKE_BITSET    HAKU_FUTEX_WAKE_BITSET
#define FUTEX_PRIVATE_FLAG   HAKU_FUTEX_PRIVATE_FLAG
#define FUTEX_CLOCK_REALTIME HAKU_FUTEX_CLOCK_REALTIME

/* The raw call: the same shape as the Linux futex(2). The timeout is a
 * relative timespec (NULL waits forever). */
int haku_futex(volatile int *address, int op, int val,
               const haku_timespec_t *timeout, int *address2, int value3);

/* Wait until the word no longer carries `expected`, or until the
 * timeout expires. The shared form is used by default: the futex is
 * keyed by its address, so a process and its child can meet on it. A
 * private futex (FUTEX_PRIVATE_FLAG) belongs to one process. */
int futex_wait(volatile int *address, int expected,
               const haku_timespec_t *timeout);
int futex_wake(volatile int *address, int count);
int futex_wait_private(volatile int *address, int expected,
                       const haku_timespec_t *timeout);
int futex_wake_private(volatile int *address, int count);

#endif
