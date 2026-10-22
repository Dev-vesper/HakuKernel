#include <haku/errno.h>
#include <haku/signal.h>
#include <haku/syscall.h>
#include <haku/unistd.h>

/* sigaction(2): the kernel keeps the kernel's i386 layout (handler, flags,
 * restorer, mask) and the mask is one word, but the call still reports the
 * kernel's eight-byte sigset_t size, which is what musl passes too. */
#define HAKU_SIGSET_SIZE 8

int kill(int pid, int sig) {
    return haku_errno_check(haku_syscall2(HAKU_SYS_KILL, pid, sig));
}

int tkill(int tid, int sig) {
    return haku_errno_check(haku_syscall2(HAKU_SYS_TKILL, tid, sig));
}

int tgkill(int tgid, int tid, int sig) {
    return haku_errno_check(
        haku_syscall3(HAKU_SYS_TGKILL, tgid, tid, sig));
}

int raise(int sig) {
    return kill(getpid(), sig);
}

sighandler_t signal(int sig, sighandler_t handler) {
    haku_sigaction_t action;
    haku_sigaction_t previous;
    action.handler = (unsigned int)handler;
    action.flags = 0;
    action.restorer = 0;
    action.mask = 0;
    action.mask_high = 0;
    if (haku_errno_check(haku_syscall4(HAKU_SYS_RT_SIGACTION, sig,
                                       (int)&action, (int)&previous,
                                       HAKU_SIGSET_SIZE)) == -1)
        return SIG_ERR;
    return (sighandler_t)previous.handler;
}

int sigaction(int sig, const haku_sigaction_t *action,
              haku_sigaction_t *previous) {
    return haku_errno_check(
        haku_syscall4(HAKU_SYS_RT_SIGACTION, sig, (int)action,
                      (int)previous, HAKU_SIGSET_SIZE));
}

int sigprocmask(int how, const sigset_t *set, sigset_t *previous) {
    return haku_errno_check(
        haku_syscall4(HAKU_SYS_RT_SIGPROCMASK, how, (int)set,
                      (int)previous, HAKU_SIGSET_SIZE));
}

int sigemptyset(sigset_t *set) {
    if (!set) return haku_errno_check((0u - HAKU_EFAULT));
    *set = 0u;
    return 0;
}

int sigfillset(sigset_t *set) {
    if (!set) return haku_errno_check((0u - HAKU_EFAULT));
    /* Only the implemented range, so the mask stays one word. */
    *set = (1u << 31) - 1u;
    return 0;
}

int sigaddset(sigset_t *set, int sig) {
    if (!set || sig < 1 || sig > 31) return haku_errno_check((0u - HAKU_EINVAL));
    *set |= sigmask(sig);
    return 0;
}

int sigdelset(sigset_t *set, int sig) {
    if (!set || sig < 1 || sig > 31) return haku_errno_check((0u - HAKU_EINVAL));
    *set &= ~sigmask(sig);
    return 0;
}

int sigismember(const sigset_t *set, int sig) {
    if (!set || sig < 1 || sig > 31) return haku_errno_check((0u - HAKU_EINVAL));
    return (*set & sigmask(sig)) != 0u;
}
