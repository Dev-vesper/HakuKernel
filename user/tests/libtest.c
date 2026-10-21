#include <haku/haku.h>

static int failures;

static void expect(int condition, const char *what) {
    if (!condition) {
        printf("USER-LIB-FAIL: %s (errno=%d)\n", what, errno);
        failures++;
    }
}

int main(void) {
    char buffer[64];
    char copy[32];
    char *pointer;
    haku_stat64_t status;
    haku_timeval_t before;
    haku_timeval_t after;
    haku_timespec_t pause;
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

    if (failures) {
        printf("USER-LIB-FAIL: %d checks failed\n", failures);
        return 1;
    }
    printf("USER-LIB-OK: syscalls strings memory file io errno (pid=%d)\n",
           getpid());
    return 0;
}
