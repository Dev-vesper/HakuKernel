#include <haku/errno.h>
#include <haku/futex.h>
#include <haku/syscall.h>

int haku_futex(volatile int *address, int op, int val,
               const haku_timespec_t *timeout, int *address2, int value3) {
    return haku_errno_check(
        haku_syscall6(HAKU_SYS_FUTEX, (int)address, op, val, (int)timeout,
                      (int)address2, value3));
}

int futex_wait(volatile int *address, int expected,
               const haku_timespec_t *timeout) {
    return haku_futex(address, HAKU_FUTEX_WAIT, expected, timeout, 0, 0);
}

int futex_wake(volatile int *address, int count) {
    return haku_futex(address, HAKU_FUTEX_WAKE, count, 0, 0, 0);
}

int futex_wait_private(volatile int *address, int expected,
                       const haku_timespec_t *timeout) {
    return haku_futex(address, HAKU_FUTEX_WAIT | HAKU_FUTEX_PRIVATE_FLAG,
                      expected, timeout, 0, 0);
}

int futex_wake_private(volatile int *address, int count) {
    return haku_futex(address, HAKU_FUTEX_WAKE | HAKU_FUTEX_PRIVATE_FLAG,
                      count, 0, 0, 0);
}
