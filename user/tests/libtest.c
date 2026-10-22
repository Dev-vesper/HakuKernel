#include <haku/haku.h>

static int failures;

static void expect(int condition, const char *what) {
    if (!condition) {
        printf("USER-LIB-FAIL: %s (errno=%d)\n", what, errno);
        failures++;
    }
}

/* Signal handlers: a plain one, and one that asks for the siginfo and
 * ucontext arguments. Delivery builds them on the user stack. */
static volatile int handler_runs;
static volatile int handler_signal;
static volatile int handler_si_signo;
static volatile int handler_info_code;
static volatile void *handler_context;

static void on_signal(int sig) {
    handler_signal = sig;
    handler_runs++;
}

static void on_signal_info(int sig, void *info, void *context) {
    handler_signal = sig;
    handler_si_signo = ((int *)info)[0];
    handler_info_code = ((int *)info)[2];
    handler_context = context;
    handler_runs++;
}

static void install(int sig, unsigned int handler, unsigned int flags,
                    unsigned int restorer) {
    haku_sigaction_t action;
    action.handler = handler;
    action.flags = flags;
    action.restorer = restorer;
    action.mask = 0;
    action.mask_high = 0;
    expect(sigaction(sig, &action, NULL) == 0, "sigaction");
}

int main(void) {
    char buffer[64];
    char copy[32];
    char *pointer;
    haku_stat64_t status;
    haku_timeval_t before;
    haku_timeval_t after;
    haku_timespec_t pause;
    sigset_t signal_set;
    sigset_t signal_old;
    void *blocks[8];
    int fd;
    int i;

    printf("USER-LIB-START pid=%d\n", getpid());

    unlink("LIB.TMP");
    unlink("LIB2.TMP");
    unlink("LIBDIR/F.TXT");
    rmdir("LIBDIR");

    strcpy(buffer, "haku");
    strcat(buffer, "-lib");
    expect(strcmp(buffer, "haku-lib") == 0, "strcpy/strcat/strcmp");
    strncpy(copy, buffer, sizeof(copy));
    expect(strncmp(copy, "haku", 4) == 0, "strncpy/strncmp");
    expect(strlen(copy) == 8u, "strlen");
    expect(strchr(copy, '-') != NULL, "strchr");
    expect(strrchr(copy, '-') != NULL, "strrchr");
    expect(strstr(copy, "lib") != NULL, "strstr");

    memset(copy, 'x', 4);
    expect(memcmp(copy, "xxxx", 4) == 0, "memset/memcmp");
    memcpy(buffer, "0123456789", 11);
    memmove(buffer + 2, buffer, 8);
    expect(memcmp(buffer, "0101234567", 10) == 0, "memmove");
    expect(memchr(buffer, '5', 11) == (void *)(buffer + 7), "memchr");

    for (i = 0; i < 8; i++) {
        blocks[i] = malloc((size_t)(16 + i * 37));
        expect(blocks[i] != NULL, "malloc");
        if (blocks[i]) {
            expect((((unsigned int)blocks[i]) & 7u) == 0u,
                   "malloc alignment");
            memset(blocks[i], i, (size_t)(16 + i * 37));
        }
    }
    pointer = (char *)realloc(blocks[3], 512);
    expect(pointer != NULL, "realloc");
    if (pointer) {
        blocks[3] = pointer;
        expect(pointer[0] == 3 && pointer[100] == 3,
               "realloc preserves data");
    }
    pointer = (char *)calloc(16, 4);
    expect(pointer != NULL, "calloc");
    if (pointer) {
        int zero = 1;
        for (i = 0; i < 64; i++) {
            if (pointer[i]) zero = 0;
        }
        expect(zero, "calloc zeroes memory");
        free(pointer);
    }
    for (i = 0; i < 8; i++) free(blocks[i]);
    expect(malloc(0xFFFFFFFFu) == NULL && errno == ENOMEM,
           "malloc overflow");

    fd = open("NOFILE.TXT", O_RDONLY);
    expect(fd == -1 && errno == ENOENT, "open ENOENT");
    expect(close(99) == -1 && errno == EBADF, "close EBADF");

    fd = open("LIB.TMP", O_CREAT | O_RDWR | O_TRUNC, 0644);
    expect(fd >= 3, "open LIB.TMP");
    if (fd >= 3) {
        const char *payload = "haku-lib-file";
        size_t length = strlen(payload);
        expect(write(fd, payload, length) == (ssize_t)length, "write");
        expect(lseek(fd, 0, SEEK_SET) == 0, "lseek");
        memset(buffer, 0, sizeof(buffer));
        expect(read(fd, buffer, sizeof(buffer)) == (ssize_t)length, "read");
        expect(memcmp(buffer, payload, length) == 0, "read data");
        expect(fstat(fd, &status) == 0, "fstat");
        expect(status.st_size == length, "fstat size");
        expect(status.st_blksize == 512, "fstat blksize");
        expect(status.st_nlink == 1, "fstat nlink");
        expect(close(fd) == 0, "close");
        expect(stat("LIB.TMP", &status) == 0, "stat");
        expect((status.st_mode & S_IFREG) != 0, "stat mode");
        expect(status.st_size == length, "stat size");
        expect(status.st_nlink == 1, "stat nlink");
        expect(status.st_uid == 0 && status.st_gid == 0, "stat owner");
        expect(status.st_blocks == (length + 511u) / 512u, "stat blocks");
        expect(rename("LIB.TMP", "LIB2.TMP") == 0, "rename");
        expect(unlink("LIB2.TMP") == 0, "unlink");
        expect(stat("LIB2.TMP", &status) == -1 && errno == ENOENT,
               "unlink removed file");
    }

    expect(gettimeofday(&before, NULL) == 0, "gettimeofday");
    pause.tv_sec = 0;
    pause.tv_nsec = 10000000u;
    expect(nanosleep(&pause, NULL) == 0, "nanosleep");
    expect(gettimeofday(&after, NULL) == 0, "gettimeofday after sleep");
    expect(after.tv_sec > before.tv_sec ||
               (after.tv_sec == before.tv_sec &&
                after.tv_usec >= before.tv_usec),
           "time advanced");

    expect(mkdir("LIBDIR") == 0, "mkdir");
    fd = open("LIBDIR/F.TXT", O_CREAT | O_RDWR | O_TRUNC, 0644);
    expect(fd >= 3, "open in directory");
    if (fd >= 3) expect(close(fd) == 0, "close in directory");
    expect(unlink("LIBDIR/F.TXT") == 0, "unlink in directory");
    expect(rmdir("LIBDIR") == 0, "rmdir");

    /* fork: the child writes a file and leaves with status 7, the parent
     * waits for it and sees the status. */
    {
        int child = fork();
        if (child == 0) {
            int child_fd = open("FORK.TXT", O_CREAT | O_RDWR | O_TRUNC, 0644);
            if (child_fd >= 3) {
                write(child_fd, "x", 1);
                close(child_fd);
            }
            exit(7);
        }
        expect(child > 0, "fork");
        if (child > 0) {
            int child_status = 0;
            int reaped = waitpid(child, &child_status, 0);
            expect(reaped == child, "waitpid child");
            expect(child_status == (7 << 8), "waitpid status");
            expect(waitpid(child, &child_status, 0) == -1 &&
                       errno == ECHILD,
                   "waitpid reaped twice");
            expect(unlink("FORK.TXT") == 0, "unlink fork file");
        }
    }

    /* vfork: the child borrows our address space, so a variable it writes
     * is visible when the parent resumes with the child pid. */
    {
        static volatile int shared_word;
        int child;
        shared_word = 0;
        child = vfork();
        if (child == 0) {
            shared_word = 1234;
            exit(5);
        }
        expect(child > 0, "vfork");
        if (child > 0) {
            int child_status = 0;
            expect(shared_word == 1234, "vfork shared memory");
            expect(waitpid(child, &child_status, 0) == child,
                   "waitpid vfork child");
            expect(child_status == (5 << 8), "vfork status");
        }
    }

    expect(atoi("-123") == -123, "atoi");

    /* Signals: install a handler, raise, and check that the kernel
     * delivered the signal number and resumed after the handler. */
    sigemptyset(&signal_set);
    expect(signal(SIGUSR1, on_signal) == SIG_DFL, "signal returns SIG_DFL");
    expect(raise(SIGUSR1) == 0, "raise SIGUSR1");
    expect(handler_runs == 1 && handler_signal == SIGUSR1, "handler ran");

    expect(signal(SIGUSR2, SIG_IGN) == SIG_DFL, "signal SIG_IGN");
    expect(raise(SIGUSR2) == 0, "raise ignored signal");
    expect(handler_runs == 1, "ignored signal not delivered");

    expect(signal(SIGKILL, on_signal) == SIG_ERR && errno == EINVAL,
           "SIGKILL cannot be caught");
    expect(signal(SIGSTOP, on_signal) == SIG_ERR && errno == EINVAL,
           "SIGSTOP cannot be caught");

    /* A blocked signal stays pending and is delivered on the unblocking
     * syscall's return. */
    sigaddset(&signal_set, SIGUSR1);
    expect(sigismember(&signal_set, SIGUSR1) == 1, "sigismember");
    expect(sigprocmask(SIG_BLOCK, &signal_set, &signal_old) == 0,
           "sigprocmask block");
    expect(sigismember(&signal_old, SIGUSR1) == 0, "previous mask empty");
    expect(raise(SIGUSR1) == 0, "raise while blocked");
    expect(handler_runs == 1, "blocked signal not delivered");
    expect(sigprocmask(SIG_UNBLOCK, &signal_set, NULL) == 0,
           "sigprocmask unblock");
    expect(handler_runs == 2 && handler_signal == SIGUSR1,
           "unblock delivers pending signal");

    /* SA_SIGINFO handlers see si_signo and a ucontext. A restorer supplied
     * by the program (musl's convention) also returns through the kernel. */
    install(SIGUSR2, (unsigned int)on_signal_info, SA_SIGINFO, 0);
    expect(raise(SIGUSR2) == 0, "raise SA_SIGINFO");
    expect(handler_runs == 3 && handler_signal == SIGUSR2 &&
               handler_si_signo == SIGUSR2 && handler_info_code == 0 &&
               handler_context != NULL,
           "SA_SIGINFO arguments");
    install(SIGUSR2, (unsigned int)on_signal, SA_RESTORER | SA_NODEFER,
            (unsigned int)haku_sigreturn_trampoline);
    expect(raise(SIGUSR2) == 0, "raise SA_RESTORER");
    expect(handler_runs == 4 && handler_signal == SIGUSR2,
           "SA_RESTORER restorer returns");

    /* The pid/tid forms. Haku has one thread per process. */
    expect(kill(getpid(), 0) == 0, "kill sig 0 self");
    expect(kill(999, 0) == -1 && errno == ESRCH, "kill ESRCH");
    expect(tkill(getpid(), 0) == 0, "tkill sig 0 self");
    expect(tgkill(getpid(), getpid(), 0) == 0, "tgkill sig 0 self");
    expect(tgkill(999, getpid(), 0) == -1 && errno == ESRCH,
           "tgkill wrong thread group");
    expect(tgkill(getpid(), 999, 0) == -1 && errno == ESRCH,
           "tgkill unknown thread");
    expect(kill(getpid(), 32) == -1 && errno == EINVAL, "kill bad signal");
    expect(kill(-2, 0) == -1 && errno == ESRCH, "kill bad group");
    expect(kill(-1, 0) == 0, "kill every other process");

    /* The default disposition of SIGUSR1 ends a process; the parent sees
     * the signal in the wait status. */
    {
        int child = fork();
        if (child == 0) {
            signal(SIGUSR1, SIG_DFL);
            raise(SIGUSR1);
            _exit(9);
        }
        expect(child > 0, "fork for signal");
        if (child > 0) {
            int child_status = 0;
            expect(waitpid(child, &child_status, 0) == child,
                   "waitpid signalled child");
            expect((child_status & 0x7F) == SIGUSR1,
                   "wait status carries SIGUSR1");
        }
    }

    /* A signal wakes a task blocked in a syscall: nanosleep reports EINTR
     * and the handler runs once the task is dispatched again. */
    {
        int child = fork();
        if (child == 0) {
            haku_timespec_t nap;
            sigemptyset(&signal_set);
            handler_runs = 0;
            expect(signal(SIGUSR1, on_signal) == SIG_DFL, "child signal");
            nap.tv_sec = 2;
            nap.tv_nsec = 0;
            expect(nanosleep(&nap, NULL) == -1 && errno == EINTR,
                   "signal interrupts nanosleep");
            nap.tv_sec = 0;
            nap.tv_nsec = 50000000u;
            nanosleep(&nap, NULL);
            _exit(handler_runs == 1 ? 0 : 1);
        }
        expect(child > 0, "fork for EINTR");
        if (child > 0) {
            haku_timespec_t nap_short;
            int child_status = 0;
            nap_short.tv_sec = 0;
            nap_short.tv_nsec = 100000000u;
            nanosleep(&nap_short, NULL);
            expect(kill(child, SIGUSR1) == 0, "kill sleeping child");
            expect(waitpid(child, &child_status, 0) == child,
                   "waitpid interrupted child");
            expect(child_status == 0, "EINTR handler ran in child");
        }
    }

    if (failures) {
        printf("USER-LIB-FAIL: %d checks failed\n", failures);
        return 1;
    }
    printf("USER-SIGNAL-OK: kill tkill tgkill sigaction sigprocmask\n");
    printf("USER-LIB-OK: syscalls strings memory file io errno (pid=%d)\n",
           getpid());
    return 0;
}
