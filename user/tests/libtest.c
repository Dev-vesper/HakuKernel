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

/* The filesystem and descriptor ABI a shell needs: the *at family, a
 * working directory, directory enumeration, pipes, poll, fcntl, termios,
 * limits, and the small process-control calls. */
static void test_filesystem_abi(void) {
    char cwd[64];
    char buffer[64];
    char name[16];
    unsigned char random_bytes[8];
    haku_stat64_t status;
    rlimit_t limits;
    termios_t attributes;
    struct winsize window;
    struct pollfd poll_fds[2];
    struct dirent *entry;
    DIR *dir;
    int fds[2];
    int fd;
    int duplicate;
    int found;

    unlink("ABI0.TXT");
    unlink("ABI1.TXT");
    unlink("ABI2.TXT");
    unlink("ABI3.TXT");
    unlink("ABIDIR/ABI0.TXT");
    unlink("ABIDIR/ABI1.TXT");
    rmdir("ABIDIR");

    expect(mkdir("ABIDIR") == 0, "mkdir for the *at tests");
    fd = open("ABIDIR", O_RDONLY | O_DIRECTORY);
    expect(fd >= 0, "open O_DIRECTORY");
    if (fd >= 0) {
        int inside = openat(fd, "ABI0.TXT", O_CREAT | O_RDWR, 0644);
        expect(inside >= 0, "openat with a dirfd");
        if (inside >= 0) {
            expect(write(inside, "hello", 5) == 5, "write after openat");
            expect(ftruncate(inside, 2) == 0, "ftruncate shrink");
            expect(lseek(inside, 0, SEEK_END) == 2, "size after ftruncate");
            close(inside);
        }
        expect(stat("ABIDIR/ABI0.TXT", &status) == 0, "stat inside a dir");
        expect(status.st_size == 2, "stat sees the truncated size");
        expect(truncate("ABIDIR/ABI0.TXT", 6) == 0, "truncate grow");
        expect(stat("ABIDIR/ABI0.TXT", &status) == 0 &&
               status.st_size == 6, "truncate grew with zeros");
        expect(renameat(fd, "ABI0.TXT", fd, "ABI1.TXT") == 0, "renameat");
        expect(unlinkat(fd, "ABI1.TXT", 0) == 0, "unlinkat");
        expect(mkdirat(fd, "SUB", 0755) == 0, "mkdirat");
        expect(unlinkat(fd, "SUB", AT_REMOVEDIR) == 0, "unlinkat rmdir");
        close(fd);
    }

    /* A working directory changes what a relative name means. */
    expect(chdir("ABIDIR") == 0, "chdir");
    expect(getcwd(cwd, sizeof(cwd)) != 0 && strcmp(cwd, "/ABIDIR") == 0,
           "getcwd after chdir");
    fd = open("ABI1.TXT", O_CREAT | O_WRONLY, 0644);
    expect(fd >= 0, "create relative to the working directory");
    if (fd >= 0) close(fd);
    expect(access("ABI1.TXT", F_OK) == 0, "access F_OK");
    expect(access("ABI1.TXT", X_OK) != 0, "access X_OK refused");
    expect(chdir("..") == 0, "chdir back");
    expect(getcwd(cwd, sizeof(cwd)) != 0 && strcmp(cwd, "/") == 0,
           "getcwd at the root");
    expect(unlink("ABIDIR/ABI1.TXT") == 0, "unlink through a path");
    expect(rmdir("ABIDIR") == 0, "rmdir");

    /* getdents64 through the directory API. */
    found = 0;
    dir = opendir("/");
    if (dir) {
        while ((entry = readdir(dir)) != 0) {
            if (strcmp(entry->d_name, "HAKU.ELF") == 0) found = 1;
        }
        closedir(dir);
    }
    expect(found == 1, "getdents64 lists HAKU.ELF");

    /* Pipes: write, readiness, read, and end of file. */
    expect(pipe(fds) == 0, "pipe");
    expect(write(fds[1], "ping", 4) == 4, "pipe write");
    poll_fds[0].fd = fds[0];
    poll_fds[0].events = POLLIN;
    poll_fds[0].revents = 0;
    poll_fds[1].fd = fds[1];
    poll_fds[1].events = POLLOUT;
    poll_fds[1].revents = 0;
    expect(poll(poll_fds, 2, 0) == 2, "poll reports two ready descriptors");
    expect((poll_fds[0].revents & POLLIN) != 0, "poll POLLIN on the pipe");
    expect(read(fds[0], buffer, 4) == 4 && buffer[0] == 'p', "pipe read");
    expect(close(fds[1]) == 0, "close the write end");
    expect(read(fds[0], buffer, 4) == 0, "pipe end of file");
    close(fds[0]);

    /* dup3 and fcntl. */
    fd = open("ABI2.TXT", O_CREAT | O_RDWR, 0644);
    expect(fd >= 0, "open for dup3");
    if (fd >= 0) {
        duplicate = dup3(fd, 7, 0);
        expect(duplicate == 7, "dup3 onto a fixed descriptor");
        if (duplicate >= 0) close(duplicate);
        expect(dup3(fd, fd, 0) == -1 && errno == EINVAL,
               "dup3 rejects identical descriptors");
        close(fd);
        unlink("ABI2.TXT");
    }
    fd = open("ABI3.TXT", O_CREAT | O_RDWR, 0644);
    expect(fd >= 0, "open for fcntl");
    if (fd >= 0) {
        expect(fcntl(fd, F_SETFL, O_APPEND) == 0, "fcntl F_SETFL");
        expect((fcntl(fd, F_GETFL, 0) & O_APPEND) != 0, "fcntl F_GETFL");
        duplicate = fcntl(fd, F_DUPFD, 5);
        expect(duplicate >= 5, "fcntl F_DUPFD");
        if (duplicate >= 0) close(duplicate);
        close(fd);
        unlink("ABI3.TXT");
    }

    /* The console answers the termios and window-size requests. */
    expect(tcgetattr(1, &attributes) == 0, "tcgetattr");
    expect((attributes.c_lflag & ECHO) != 0, "termios reports ECHO");
    expect(tcsetattr(1, 0, &attributes) == 0, "tcsetattr");
    expect(ioctl(1, TIOCGWINSZ, &window) == 0 && window.ws_col == 80,
           "TIOCGWINSZ");
    expect(isatty(1) == 1, "isatty(1)");
    expect(isatty(-1) == 0, "isatty(-1)");

    /* Limits, process name, persona, and randomness. */
    expect(getrlimit(RLIMIT_NOFILE, &limits) == 0 && limits.rlim_cur == 8,
           "getrlimit NOFILE");
    limits.rlim_cur = 4;
    expect(setrlimit(RLIMIT_NOFILE, &limits) == 0, "setrlimit");
    memcpy(name, "haku-test", 10);
    expect(prctl(PR_SET_NAME, (unsigned int)name, 0, 0, 0) == 0,
           "PR_SET_NAME");
    memset(name, 0, sizeof(name));
    expect(prctl(PR_GET_NAME, (unsigned int)name, 0, 0, 0) == 0 &&
               strcmp(name, "haku-test") == 0, "PR_GET_NAME");
    expect(personality(PER_QUERY) == PER_LINUX, "personality query");
    expect(getrandom(random_bytes, sizeof(random_bytes), 0) ==
               (int)sizeof(random_bytes), "getrandom");
    expect(sched_yield() == 0, "sched_yield");
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

    /* The memory syscalls a libc needs: a fixed anonymous mapping, the
     * protection changes, moving a mapping, the advice call, and the
     * thread pointer. */
    {
        unsigned char *fixed = (unsigned char *)0x64000000u;
        unsigned char *first;
        unsigned char *second;
        unsigned char *grown;
        int i;

        /* mmap2 with MAP_FIXED places the mapping at the given address;
         * the pages come back zero-filled. */
        expect(mmap2(fixed, 0x2000u, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) ==
                   (void *)fixed,
               "mmap2 MAP_FIXED");
        for (i = 0; i < 0x2000; i++) {
            if (fixed[i] != 0) break;
        }
        expect(i == 0x2000, "fixed mapping is zero filled");
        fixed[0] = 0x5Au;
        fixed[0x1FFF] = 0xA5u;
        expect(fixed[0] == 0x5Au && fixed[0x1FFF] == 0xA5u,
               "fixed mapping is writable");

        /* mprotect to read-only keeps the contents readable. */
        expect(mprotect(fixed, 0x2000u, PROT_READ) == 0, "mprotect read");
        expect(fixed[0] == 0x5Au, "read-only mapping is readable");

        /* A write to a read-only mapping faults, which ends the process:
         * the child does it and the parent sees the fault status. */
        {
            int child = fork();
            if (child == 0) {
                fixed[0] = 1u;
                _exit(0);
            }
            expect(child > 0, "fork for protection fault");
            if (child > 0) {
                int child_status = 0;
                expect(waitpid(child, &child_status, 0) == child,
                       "waitpid protection fault");
                expect(((child_status >> 8) & 0x80u) != 0u,
                       "write to a read-only mapping faults");
            }
        }

        /* Back to read/write, then madvise releases the pages: the next
         * access sees zeroed memory again. */
        expect(mprotect(fixed, 0x2000u, PROT_READ | PROT_WRITE) == 0,
               "mprotect write");
        expect(madvise(fixed, 0x2000u, MADV_DONTNEED) == 0, "madvise");
        expect(fixed[0] == 0 && fixed[0x1FFF] == 0,
               "madvise releases the pages");
        expect(madvise(fixed, 0x2000u, 99) == -1 && errno == EINVAL,
               "madvise rejects an unknown advice");

        /* mremap: without MAYMOVE a mapping cannot grow into a taken
         * address, and with it the mapping moves and keeps its contents. */
        first = (unsigned char *)mmap2(NULL, 0x1000u, PROT_READ | PROT_WRITE,
                                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        second = (unsigned char *)mmap2(NULL, 0x1000u, PROT_READ | PROT_WRITE,
                                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        expect(first != MAP_FAILED && second != MAP_FAILED, "mmap2 pair");
        first[0] = 0x11u;
        expect(mremap(first, 0x1000u, 0x2000u, 0, NULL) == MAP_FAILED &&
                   errno == EINVAL,
               "mremap without MAYMOVE cannot grow");
        grown = (unsigned char *)mremap(first, 0x1000u, 0x2000u,
                                        MREMAP_MAYMOVE, NULL);
        expect(grown != MAP_FAILED, "mremap");
        if (grown != MAP_FAILED) {
            expect(grown[0] == 0x11u, "mremap keeps the contents");
            grown[0x1000] = 0x33u;
            expect(grown[0x1000] == 0x33u, "mremap grows the mapping");
        }
        expect(munmap(second, 0x1000u) == 0, "munmap second");
        expect(munmap(fixed, 0x2000u) == 0, "munmap");
        expect(munmap(fixed, 0x2000u) == -1 && errno == EINVAL,
               "munmap of a released range");
    }

    /* The thread pointer: set_thread_area moves the user FS base, so
     * %fs:0 points at the thread block the program set up. */
    {
        static unsigned int thread_block[4];
        haku_thread_area_t area;
        unsigned int seen = 0;

        thread_block[0] = (unsigned int)&thread_block[0];
        area.entry_number = 0;
        area.base_addr = (unsigned int)&thread_block[0];
        expect(set_thread_area(&area) == 0, "set_thread_area");
        __asm {
            mov eax, 0
            mov ebx, fs:[eax]
            mov seen, ebx
        }
        expect(seen == (unsigned int)&thread_block[0],
               "the FS base is the thread pointer");
        area.base_addr = 0;
        expect(set_thread_area(&area) == 0, "set_thread_area reset");
    }

    /* futex: a task parks on a word until another task wakes it. */
    {
        static volatile int futex_word;
        haku_timespec_t timeout;

        /* A word that does not carry the expected value is not waited on
         * at all: the call reports EAGAIN. */
        futex_word = 1;
        expect(futex_wait(&futex_word, 0, NULL) == -1 && errno == EAGAIN,
               "futex wait on a changed word");
        futex_word = 0;
        expect(futex_wake(&futex_word, 1) == 0, "futex wake without waiters");

        /* A wait that runs out of time reports ETIMEDOUT. */
        timeout.tv_sec = 0;
        timeout.tv_nsec = 50000000u;
        expect(futex_wait(&futex_word, 0, &timeout) == -1 &&
                   errno == ETIMEDOUT,
               "futex wait times out");

        /* The child parks on the word and the parent wakes it. */
        {
            int child = fork();
            if (child == 0) {
                haku_timespec_t child_timeout;
                child_timeout.tv_sec = 5;
                child_timeout.tv_nsec = 0;
                if (futex_wait(&futex_word, 0, &child_timeout) == 0)
                    _exit(0);
                _exit(1);
            }
            expect(child > 0, "fork for futex");
            if (child > 0) {
                haku_timespec_t nap;
                int child_status = 0;
                nap.tv_sec = 0;
                nap.tv_nsec = 200000000u;
                nanosleep(&nap, NULL);
                futex_word = 1;
                expect(futex_wake(&futex_word, 1) == 1, "futex wake");
                expect(waitpid(child, &child_status, 0) == child,
                       "waitpid futex child");
                expect(child_status == 0, "the futex waiter woke up");
            }
        }

        /* A signal ends a futex wait with EINTR. */
        {
            int child;
            futex_word = 0;
            child = fork();
            if (child == 0) {
                signal(SIGUSR1, on_signal);
                if (futex_wait(&futex_word, 0, NULL) == -1 &&
                    errno == EINTR)
                    _exit(0);
                _exit(1);
            }
            expect(child > 0, "fork for the interrupted futex");
            if (child > 0) {
                haku_timespec_t nap;
                int child_status = 0;
                nap.tv_sec = 0;
                nap.tv_nsec = 200000000u;
                nanosleep(&nap, NULL);
                expect(kill(child, SIGUSR1) == 0, "signal the futex waiter");
                expect(waitpid(child, &child_status, 0) == child,
                       "waitpid interrupted waiter");
                expect(child_status == 0, "the futex wait returned EINTR");
            }
        }

        /* A task that dies while waiting leaves the queue behind: the
         * wake finds nobody. */
        {
            int child;
            futex_word = 0;
            child = fork();
            if (child == 0) {
                futex_wait(&futex_word, 0, NULL);
                _exit(1);
            }
            expect(child > 0, "fork for the killed futex waiter");
            if (child > 0) {
                haku_timespec_t nap;
                int child_status = 0;
                nap.tv_sec = 0;
                nap.tv_nsec = 200000000u;
                nanosleep(&nap, NULL);
                expect(kill(child, SIGKILL) == 0, "kill the futex waiter");
                expect(waitpid(child, &child_status, 0) == child,
                       "waitpid killed waiter");
                expect((child_status & 0x7F) == SIGKILL,
                       "the killed waiter reports SIGKILL");
                expect(futex_wake(&futex_word, 1) == 0,
                       "the dead waiter left the queue");
            }
        }
    }

    test_filesystem_abi();

    if (failures) {
        printf("USER-LIB-FAIL: %d checks failed\n", failures);
        return 1;
    }
    printf("USER-FS-ABI-OK: openat getdents64 chdir pipe poll fcntl termios\n");
    printf("USER-SIGNAL-OK: kill tkill tgkill sigaction sigprocmask\n");
    printf("USER-MMAP-OK: mmap2 mprotect mremap madvise set_thread_area\n");
    printf("USER-FUTEX-OK: futex wait wake timeout\n");
    printf("USER-LIB-OK: syscalls strings memory file io errno (pid=%d)\n",
           getpid());
    return 0;
}
