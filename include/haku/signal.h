#ifndef HAKU_SIGNAL_H
#define HAKU_SIGNAL_H

#include <haku/types.h>
#include <haku/abi.h>

/* The signal numbers double as the HAKU_SIG* kernel constants. Signal
 * numbers 1..31 are implemented; the real-time range is not. */
typedef unsigned int sigset_t;
typedef void (*sighandler_t)(int);

#define SIG_DFL ((sighandler_t)HAKU_SIG_DFL)
#define SIG_IGN ((sighandler_t)HAKU_SIG_IGN)
#define SIG_ERR ((sighandler_t)-1)

#define SIG_BLOCK HAKU_SIG_BLOCK
#define SIG_UNBLOCK HAKU_SIG_UNBLOCK
#define SIG_SETMASK HAKU_SIG_SETMASK

#define SA_NOCLDSTOP HAKU_SA_NOCLDSTOP
#define SA_SIGINFO HAKU_SA_SIGINFO
#define SA_RESTORER HAKU_SA_RESTORER
#define SA_RESTART HAKU_SA_RESTART
#define SA_NODEFER HAKU_SA_NODEFER

#define sigmask(sig) (1u << ((unsigned int)(sig) - 1u))

#define SIGHUP HAKU_SIGHUP
#define SIGINT HAKU_SIGINT
#define SIGQUIT HAKU_SIGQUIT
#define SIGILL HAKU_SIGILL
#define SIGTRAP HAKU_SIGTRAP
#define SIGABRT HAKU_SIGABRT
#define SIGBUS HAKU_SIGBUS
#define SIGFPE HAKU_SIGFPE
#define SIGKILL HAKU_SIGKILL
#define SIGUSR1 HAKU_SIGUSR1
#define SIGSEGV HAKU_SIGSEGV
#define SIGUSR2 HAKU_SIGUSR2
#define SIGPIPE HAKU_SIGPIPE
#define SIGALRM HAKU_SIGALRM
#define SIGTERM HAKU_SIGTERM
#define SIGSTKFLT HAKU_SIGSTKFLT
#define SIGCHLD HAKU_SIGCHLD
#define SIGCONT HAKU_SIGCONT
#define SIGSTOP HAKU_SIGSTOP
#define SIGTSTP HAKU_SIGTSTP
#define SIGTTIN HAKU_SIGTTIN
#define SIGTTOU HAKU_SIGTTOU
#define SIGURG HAKU_SIGURG
#define SIGXCPU HAKU_SIGXCPU
#define SIGXFSZ HAKU_SIGXFSZ
#define SIGVTALRM HAKU_SIGVTALRM
#define SIGPROF HAKU_SIGPROF
#define SIGWINCH HAKU_SIGWINCH
#define SIGIO HAKU_SIGIO
#define SIGPWR HAKU_SIGPWR
#define SIGSYS HAKU_SIGSYS

int kill(int pid, int sig);
int tkill(int tid, int sig);
int tgkill(int tgid, int tid, int sig);
int raise(int sig);
sighandler_t signal(int sig, sighandler_t handler);
int sigaction(int sig, const haku_sigaction_t *action,
              haku_sigaction_t *previous);
int sigprocmask(int how, const sigset_t *set, sigset_t *previous);
int sigemptyset(sigset_t *set);
int sigfillset(sigset_t *set);
int sigaddset(sigset_t *set, int sig);
int sigdelset(sigset_t *set, int sig);
int sigismember(const sigset_t *set, int sig);

/* A restorer for programs that pass SA_RESTORER themselves (musl does).
 * The kernel installs its own stub when no restorer is given. */
void haku_sigreturn_trampoline(void);

#endif
