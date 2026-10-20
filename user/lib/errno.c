#include <haku/errno.h>
#include <haku/syscall.h>

int errno;

int haku_errno_check(int result) {
    if (result < 0) {
        errno = -result;
        return -1;
    }
    return result;
}
