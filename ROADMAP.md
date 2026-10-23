# HakuKernel Roadmap

This roadmap separates implemented capabilities from planned work. HakuKernel
aims to be a small, testable x86 operating system that grows toward running
independent programs and providing a POSIX-inspired environment. The long
term direction is a Linux-compatible syscall ABI plus a statically linked
GNU userland (musl, e.g. busybox) booted from an initramfs; full POSIX
compatibility is not a short-term goal.

## Current Status

- [x] BIOS boot into 32-bit protected mode, with a bootloader that can load a
  kernel larger than the original boot-sector limit.
- [x] E820-backed physical memory inventory, two-level paging with a 128 MiB
  supervisor direct map, and separate user address spaces.
- [x] Static ELF32 programs running at ring 3, per-process kernel stacks, and
  PIT-driven preemptive scheduling.
- [x] `exit`, `read`, `write`, `open`, `close`, `lseek`, and `getpid` syscalls
  with user-pointer validation and `-errno` error returns.
- [x] Process lifecycle syscalls: `spawn`, `waitpid`, `execve`, `dup`/`dup2`,
  and per-process descriptors with inheritance and close-on-exit.
- [x] User virtual memory: a `brk` heap that grows and shrinks, anonymous
  private `mmap`, and `munmap` that trims, splits, and releases mappings for
  later reuse.
- [x] Time and accounting: `gettimeofday`, `nanosleep`, and the Haku-specific
  `getusage` snapshot of per-process memory and CPU usage.
- [x] ATA driver for both legacy channels (primary/secondary) and all
  master/slave positions, LBA28 with LBA48 fallback, bounded polling with
  software-reset retry recovery, and FLUSH CACHE after writes.
- [x] Writable FAT12 for 8.3 files and subdirectories (paths with `.`/`..`):
  create, read, write, truncate, append, mkdir/rmdir, with a VFS and
  per-process file descriptors.
- [x] QEMU headless integration tests covering multi-sector I/O, seeking,
  append, descriptor access checks, spawn/waitpid exit status, exec
  argument/environment stacks, per-process usage accounting, and persisted
  disk data.

## Proposed Milestones

### 1. Storage Reliability — Immediate Priority

- [x] Return distinct, correct errors for FAT12/VFS I/O failures, missing
  files, full disks, and invalid input (`FS-ERRORS-OK`/`FS-ENOSPC-OK` in
  headless: ENOENT/EINVAL/EEXIST/EBADF/ENOSPC/EIO mapping plus a
  root-full probe; syscall checks cover bad flags, bad fd, bad whence).
- [x] Add FAT12 tests for corrupt or cyclic chains, full disks, empty files,
  sector boundaries, and ATA errors (`FS-FILE-EDGE-OK`, `FS-CORRUPT-OK`,
  `FS-ATA-OK`: empty files, exact-sector/span/partial I/O, injected
  cut/bad/reserved/cyclic chains with restore, crafted-full data area,
  ATA bounds and NULL-transfer rejection).
- [x] Clean up allocated clusters and metadata as far as possible after failed
  writes, and document that operations are not atomic (`FS-ROLLBACK-OK`:
  gap failures keep the old size, short writes keep their durable prefix,
  directory-persist failures roll the chain back, failed TRUNC zeroes the
  entry; README documents the non-atomic model and its limits).
- [x] Avoid overwriting user data images during every build; separate initial
  image creation from preservation of an existing data image (existing
  `build/haku_fs.img` is reused as-is; only missing images or `--fresh`
  recreate it; `user.asm`-newer staleness warns; `headless` always starts
  fresh for deterministic fault-injection state).
- [x] Provide a way to inspect filesystem images after QEMU shuts down
  (`tools/lsfat12.py` + `python build.py inspect`: root listing, chains,
  `--cat`/`--dump` extraction; `headless` runs `--check` on the post-run
  image and fails the gate on mirrors/chains/orphan damage).

**Acceptance criteria:** QEMU success and fault-injection tests demonstrate
that failed operations do not leave leaked descriptors, corrupt FAT chains, or
unexpectedly lost image data across restarts.

### 2. VFS and Filesystem Completion

- [x] Define a filesystem-independent VFS interface and route descriptor
  operations through it (`vfs_fs_ops` mount/open/close/read/write/seek;
  FAT12 lives behind `fat12_vfs.c`, VFS holds no FAT12 references; boot
  registers the backend and mounts it as root; self-test guards the
  registry and proves the root survives bad mounts).
- [x] Add `unlink`, `rename`, `stat`, and `fstat`, including defined behavior
  for open files that are removed or renamed (syscalls 10/38/106/108;
  unlink defers the cluster free to the last open descriptor, rename
  repoints live descriptors; rename-to-existing gives `EEXIST`, same-name
  is a no-op; proven end to end from ring 3 as `USER-FILE-OPS-OK`, and
  the post-run image check confirms zero orphaned clusters).
- [x] Add subdirectories and basic paths, including rules for `.` and `..`
  (syscalls `mkdir`/`rmdir` 39/40; `/`-separated 8.3 paths resolved with
  real `.`/`..` entries, parent climbing clamped at the root; open/stat/
  unlink/rename are path-aware; `stat` reports `S_IFDIR`; directories are
  single-cluster and do not grow yet; proven from ring 3 as `USER-DIR-OK`
  and the host-side inspector now recurses into subdirectories).
- [x] Implement and test relevant POSIX errors such as `EEXIST`, `ENOENT`,
  `ENOSPC`, `ENOTDIR`, `EISDIR`, and `EROFS` (all covered, plus
  `ENOTEMPTY`; root mounts can be read-only via `VFS_MOUNT_RDONLY`, which
  rejects create/write/truncate/unlink/rename/mkdir/rmdir with `EROFS`
  while reads keep working, tested as `FS-EROFS-OK`).
- [ ] Introduce appropriate disk and file locks instead of disabling
  interrupts for the duration of all I/O; add a sector cache if needed.
- [ ] Decide separately whether to support long filenames, partition tables,
  or FAT16; require interoperability tests before expanding scope.

**Acceptance criteria:** Multiple processes can use independent files
concurrently; file and directory operations, EOF, seeking, and errors are
tested, and common FAT tools can still read the disk image.

### 3. Processes, Syscalls, and Program Execution

- [x] Replace the fixed test-program list with `execve` to load and replace
  the current process image from an ELF file on disk (`NEXT.ELF` headless
  test; PID and open descriptors survive replacement; argc/argv/envp are
  built and validated on the new user stack).
- [x] Add `waitpid`, a simpler ELF-based `spawn` process-creation model, and
  process exit status management (blocking parent wait, reaped child records,
  and encoded status validated in QEMU).
- [x] Define the initial user-stack convention for `argc`, `argv`, `envp`,
  and the terminating `AT_NULL` auxiliary-vector entry.
- [x] Add standard descriptors 0/1/2, descriptor inheritance or transfer, and
  automatic close-on-exit behavior (a registered console device backs fds
  0/1/2; `dup2` redirects and `close` revokes them; spawned children inherit
  the parent's descriptors, and exit releases them).
- [x] Add `dup`/`dup2` with a shared, refcounted open-file-description
  (`USER-DUP-OK`: shared offset, surviving duplicate, EBADF cases).
- [x] Add limited `brk`/`mmap` (per-process heap break plus an anonymous
  private `mmap` using the i386 old_mmap argument block; both map zero-filled
  user pages from reserved regions; `brk` also shrinks and releases pages, and
  `munmap` (91) trims, splits, and frees mappings so later `mmap` calls reuse
  the address space; `USER-MEM-OK`).
- [x] Add time syscalls such as `gettimeofday`/`nanosleep` (`gettimeofday`
  reads the PIT tick counter; `nanosleep` parks the task until its wake tick
  in the timer IRQ instead of busy-waiting; `USER-TIME-OK`).
- [x] Define a public user/kernel C ABI header for syscall numbers, errno
  values, open flags, and the `stat` structure.
- [ ] Test invalid arguments, permissions, and device errors for every syscall.

**Acceptance criteria:** A basic shell can launch programs from disk, collect
their output and exit status, and run multiple programs without unintended
address-space or descriptor sharing.

### 4. Concurrency and Memory Management

- [x] Add kernel synchronization primitives (spinlocks and IRQ-save locks) and
  define locking rules for the scheduler, allocators, VFS, and drivers
  (`kernel/kernel/sync.h`/`.c`: `irq_lock`/`irq_unlock`, task-context
  `spin_lock`/`spin_unlock`, and `spin_lock_irqsave`; VFS uses one
  task-context lock around descriptor/OFD changes and backend calls, the
  frame/heap allocators and console use IRQ-save locks, ATA and the keyboard
  ring buffer are serialized per the README "Locking" rules).
- [x] Separate wait queues and sleep/wakeup from busy-waiting so blocking I/O
  sleeps the current process instead of spinning (`kernel/kernel/scheduler.h`
  wait queues use a task-slot bitmap and a distinct `TASK_BLOCKED` state;
  blocking console reads park until IRQ1 wakes them, proven by
  `SCHED-WAITQ-OK`, while `nanosleep`/`waitpid` keep their existing states).
- [x] Extend allocation beyond the current identity-mapped range and add
  user virtual-memory allocation and release (the frame allocator uses every
  tracked E820 frame below 128 MiB through a supervisor direct map at
  `0xF0000000`; its self-test allocates and touches a frame above 8 MiB.
  `brk` now shrinks and unmaps released heap pages, and `munmap` (91) trims,
  splits, and frees anonymous mappings so later `mmap` calls reuse the
  address space; `USER-MEM-OK` covers shrink, release, and reuse).
- [x] Improve page protections and test read-only, unmapped, and
  use-after-free access (the kernel sets `CR0.WP`, maps `mmap` pages from
  `PROT_WRITE` so `PROT_READ` is enforced read-only, and only tears down user
  PDEs/PTEs; `USER-PROT-OK` spawns a
  child per case: store into an `RX` segment, store into a `PROT_READ`
  mapping, read from unmapped memory, and store through a `munmap`ed page,
  checked against `#PF` error codes 7, 7, 4, and 6). A `PROT_NONE`
  reservation was rejected at that point; the memory-syscall item below
  accepts it and makes the range fault on access.
- [x] Track memory and resource usage per process (paging maintains
  mapped-page, page-table, and peak counters per address space; the scheduler
  attributes timer ticks by CPL and counts syscalls plus voluntary and
  involuntary switches; `getusage` (0x1001) returns a 64-byte snapshot;
  `USER-USAGE-OK` covers growth, release, tick, syscall, and peak deltas).

**Acceptance criteria:** A process blocked on I/O does not prevent other
processes from making progress; allocator stress tests complete without frame
leaks or page-table corruption.

### 5. Hardware and User Experience

- [x] Define a terminal and text-I/O abstraction, separating keyboard input
  from syscalls and display backends (`kernel/drivers/terminal.c/.h` owns the
  output path and the input ring/wait queue; `kbd.c` only translates scancodes;
  the VFS console device forwards to `terminal_read`/`terminal_write_bytes`).
- [x] Add framebuffer support only with a boot protocol or safe discovery of
  the framebuffer mode and memory; retain VGA text as a fallback
  (`display.h` defines the shared `display_ops_t` API implemented by
  `vga_text.c` and `framebuffer.c`; the boot path selects the framebuffer
  backend after `framebuffer_init` succeeds, otherwise VGA text stays active).
- [x] Improve ATA timeout and recovery behavior and support more devices or
  controllers; consider IRQ/DMA instead of PIO polling if required
  (`kernel/drivers/ata.c` scans both legacy channels and all master/slave
  positions, rejects floating-bus and ATAPI devices, uses bounded polling
  with software-reset retry recovery, and flushes the write cache; FAT12
  mounts from any detected disk; IRQ/DMA completion is deliberately deferred
  because the FAT mount runs before the PIC/PIT exist, so the synchronous
  path stays deterministic).
- [x] Add leveled logging, a panic handler, and useful crash reports in QEMU
  (`klog` exposes DEBUG/INFO/WARN/ERROR/CRIT with an INFO threshold and a
  filtering self-test; `panic.c` halts on fatal faults and boot failures;
  `crash.c` prints CR0/CR2/CR3, the exception frame, registers, and the
  validated stack to the display and `build/serial.log`; ring-3 faults reuse
  the report without halting the kernel).

**Acceptance criteria:** The system remains diagnosable without graphics;
display backends use a shared API, and missing framebuffer support does not
prevent boot.

### 6. Developer Tooling and Compatibility

- [x] Provide a small user-space library and headers for syscalls, strings,
  memory, and file I/O (`include/haku/` defines the C API; `user/lib/` adds
  raw `int 0x80` stubs, errno-translating POSIX wrappers, `mem*`/`str*`, a
  first-fit `malloc` over `brk`, and a small `printf`).
- [x] Add a build target for user C programs and document a stable toolchain
  and ABI (`build.py` compiles `user/lib/` and `user/tests/libtest.c` with the
  kernel `cl.exe`/`clang-cl`, links a fixed-base PE32 without a CRT, and
  `tools/pe2elf.py` emits the ELF32 `PT_LOAD` image installed as `LIB.ELF`;
  `NEXT.ELF` spawns it in the headless run and checks `USER-LIB-OK`).
- [ ] Add automated tests for malformed ELF files, syscalls, VFS, FAT, and the
  scheduler.
- [x] Define the limited POSIX subset actually supported; add conformance
  tests and document limitations for each capability (README's "Supported
  POSIX subset" lists the implemented calls by area, what is deliberately
  absent, and which calls are accepted without changing state; the ring-3
  self-tests `USER-LIB-OK` and `USER-FS-ABI-OK` exercise the surface, though
  there is no formal conformance suite).
- [x] Document manual testing and disk-image recovery or replacement
  (README's "Manual testing and image recovery": a windowed run for
  hands-on testing, `--fresh` to rebuild a discarded data image, and
  `inspect --check` to prove an image is damaged before throwing it away).

**Acceptance criteria:** An independent C program can be built and run using
the documented toolchain, and `python build.py headless` runs all regression
tests repeatably.

### 7. Linux-Compatible ABI and a GNU Userland

Goal: run a real GNU userland (statically linked musl, e.g. busybox) on top
of HakuKernel. glibc is out of scope: it needs the 64-bit `*64` syscall
variants, `rseq`/`membarrier`, and a much wider syscall surface. The
userland root becomes a Unix-like RAM filesystem, not FAT12.

- [x] Publish a Linux-compatible syscall ABI for i386 (keep the numbers that
  already match) and a real `struct stat` layout, so unmodified
  Linux-targeted binaries can call the kernel
  (`include/haku/abi.h` lists the i386 Linux numbers, the implemented ones
  plus the ones the later milestones add; `haku_stat_t` is the 64-byte i386
  `struct stat`, `haku_stat64_t` the 96-byte `struct stat64` that glibc and
  musl use on i386, and `haku_statx_t` the 256-byte `struct statx`; the
  kernel implements `stat`/`lstat`/`fstat`, `stat64`/`lstat64`/`fstat64`,
  `fstatat64`, and `statx`, and the user library reads the same layout as
  musl; the Haku-specific `spawn`/`getusage` pair stays outside the Linux
  range).
- [x] Add process creation and reaping: `fork` (a private copy of the address
  space, the caller's saved register frame with `eax` zeroed, and the
  descriptor table), the `SIGCHLD` form of `clone`, `wait4` (`WNOHANG` and
  any-child forms; `rusage` is accepted but zeroed), and `exit_group`. The
  ELF-only `spawn` model is no longer the only way to start a process; it is
  covered by `USER-FORK-OK` in the ring-3 self-test and by the C library
  (whose `waitpid` is now a `wait4` wrapper).
- [x] Add `vfork` (share the address space until `execve`/`exit`) and
  copy-on-write for `fork` instead of the eager copy, so large processes do
  not pay for a full duplicate. Pages are shared with per-frame reference
  counts and broken on the write-protection fault, for kernel-side copies
  too; a `vfork` parent stays suspended until the child execs or exits and
  then resumes with the child PID. The library's `vfork` keeps its return
  address in `edx` across the syscall because the child runs on the caller's
  stack. Covered by `USER-COW-OK`, `USER-VFORK-OK`, `VFORK-EXEC-RAN`, and a
  `vfork` case in the C library self-test. `clone` still rejects the
  thread-creating flags, stack pointers, and parent-tid arguments with
  `EINVAL`.
- [x] Add signals: `kill`/`tkill`/`tgkill`, `rt_sigaction`,
  `rt_sigprocmask`, `rt_sigreturn`, pending-signal delivery, and default
  dispositions. Each task keeps a pending set, a blocked set, and one
  disposition per signal (a single word each, so signals 1..31 exist and
  the real-time range is rejected with `EINVAL`). Delivery happens on the
  way back to user mode: the kernel builds a frame on the user stack
  (restorer, handler arguments, saved registers, `siginfo`, `ucontext`),
  enters the handler, and `rt_sigreturn` restores the interrupted context.
  `SA_SIGINFO` and `SA_RESTORER` are honored (without a program restorer
  the kernel installs a stub that calls `rt_sigreturn`); `SIG_IGN`
  discards, `SIG_DFL` ends the process for every signal except `SIGCHLD`,
  `SIGURG`, `SIGWINCH`, and `SIGCONT`, and a parent sees the signal number
  in the wait status. A signal queued for a task blocked in a syscall
  wakes it with `EINTR`. `fork`/`vfork` reset the dispositions and keep the
  blocked set; `execve` resets the dispositions and keeps the blocked set
  and the pending signals. Covered by `USER-SIGNAL-OK` in the C library
  self-test. Still missing: process groups, job control, `sigqueue`, and
  real-time signals.
- [x] Add the memory syscalls a libc needs: `mmap2` with
  `MAP_FIXED`/`MAP_ANONYMOUS`/`MAP_SHARED`, `mprotect`, `mremap`,
  `madvise`, and thread-local storage through `set_thread_area` (today
  `mmap` only accepts `MAP_PRIVATE|MAP_ANONYMOUS` and rejects fixed
  addresses).
  `mmap` (the old argument block) and `mmap2` (the register form) now
  share one path: anonymous pages with the full protection set, a
  `MAP_FIXED` placement inside the mmap window, and the shared/private
  flag. `mprotect` changes the `R/W` bit of a range or clears the user
  bit for `PROT_NONE`; a copy-on-write page keeps its shared frame
  protected and remembers that its mapping is writable, so the first
  write still copies. `mremap` grows, shrinks, or moves a mapping
  (`MREMAP_MAYMOVE` copies the contents through the direct map),
  `MADV_DONTNEED` releases the pages so they come back zeroed, and
  `set_thread_area` points the user `FS` segment at the thread block,
  which is the thread pointer a libc reads through `%fs:0`. The library
  gains `include/haku/mman.h` and `user/lib/mman.c`; the ring-3
  self-test covers the fixed mapping, the protection changes, the fault
  on a read-only page, the move, the advice call, and the thread pointer
  (`USER-MMAP-OK`). Still missing: file-backed mappings (there is no
  page cache yet), `mremap` with `MREMAP_DONTUNMAP`, and shared
  mappings that a second task can observe.
- [x] Add `futex` so libc locks, and later threads, behave correctly
  (`FUTEX_WAIT`/`FUTEX_WAKE` plus the `_PRIVATE` and `_BITSET` forms, with a
  relative timeout that reports `ETIMEDOUT`; waiters park on the scheduler's
  address queue instead of spinning, and the C library covers the value
  check, the wake, and the timeout as `USER-FUTEX-OK`). Thread creation
  itself stays out of scope: `clone` still rejects the thread flags.
- [x] Extend the filesystem ABI for libc and shell tooling: the `*at` family
  (`openat`, `unlinkat`, `renameat`, `mkdirat`, `faccessat`, and
  `fstatat` through the existing `fstatat64`/`statx`), `fchmod`/`fchown`
  and their `*at` forms, `truncate`/`ftruncate`, `access`, `getdents64`,
  `chdir`/`getcwd`, `dup3`, `pipe`/`pipe2`, `fcntl`, `ioctl` (termios and
  `TIOCGWINSZ` for an interactive shell), `poll`, `getrlimit`/`setrlimit`/
  `prlimit64`, `getrandom`, `sched_yield`, `prctl`, and `personality`.
  Every path syscall resolves a relative name against the task's working
  directory, which `chdir`/`getcwd` keep as a canonical path, and a `dirfd`
  argument resolves against the directory descriptor the caller opened.
  `getdents64` packs Linux `linux_dirent64` records from a new backend
  `opendir`/`readdir` pair, `pipe` uses two descriptions with separate
  reader and writer counts (end of file once every write end closes,
  `EPIPE` plus `SIGPIPE` once every read end does), `fcntl` reports and sets
  `O_APPEND`/`O_NONBLOCK` and duplicates onto a requested descriptor, and
  the console answers `TCGETS`/`TCSETS*`, `TIOCGWINSZ`, and `FIONREAD`.
  Permission and ownership calls succeed without changing anything because
  FAT12 has no mode or owner bits, and `getrlimit` reports the fixed table
  sizes (8 descriptors, 4 tasks). Proven from ring 3 as `USER-FS-ABI-OK`.
  Still missing: `symlink`/`readlink` (there is no link type to store),
  `select` (musl implements it over `poll`), the old `getdents` format,
  blocking `poll` (it reports current readiness and returns immediately),
  and real record locks (`F_SETLK` is accepted as a no-op).
- [ ] Add a Unix-like filesystem for the userland: an in-memory filesystem
  (initramfs) with long names, permission bits, symlinks, timestamps, and
  nested directories, plus a device layer (`/dev/null`, `/dev/zero`,
  `/dev/urandom`, `/dev/console`, `/dev/tty`) and a minimal `/proc`
  (FAT12 stays as the data-disk backend for the existing 8.3 tree).
- [ ] Load a userland image at boot: unpack an initramfs/cpio archive into
  the RAM filesystem and `execve` `/init` as PID 1 (the boot path currently
  loads a single ELF from the FAT12 disk).
- [ ] Build the userland in the Linux CI (static musl, e.g. busybox and/or
  GNU coreutils), ship it with the release artifacts, and extend the
  headless test to assert its behavior (for example `busybox sh -c "echo"`
  and a file round-trip through the RAM filesystem).

**Acceptance criteria:** A statically linked musl binary runs as PID 1 from
an initramfs, executes shell builtins and applets, reads and writes files
in the RAM filesystem, and the headless test asserts its output; glibc,
dynamic linking, and shared libraries stay out of scope.

## Out of Scope for This Roadmap

These features require separate milestones and design decisions:

- SMP, 64-bit/long mode, and architectures other than x86.
- Networking, TCP/IP, USB, audio, and a complete graphical interface.
- Full POSIX and glibc compatibility, a dynamic linker, and shared
  libraries; the userland target is a statically linked musl userland
  (see Milestone 7).
- Journaling or transactional guarantees and power-loss recovery.

## Delivery Process

Keep each milestone small and reversible, update relevant documentation, and
validate changes with the build and headless tests. Move an item to **Current
Status** only after it has a repeatable test and its limitations are documented.
