# HakuKernel Roadmap

This roadmap separates implemented capabilities from planned work. HakuKernel
aims to be a small, testable x86 operating system that grows toward running
independent programs and providing a POSIX-inspired environment. Full POSIX
compatibility is not a short-term goal.

## Current Status

- [x] BIOS boot into 32-bit protected mode, with a bootloader that can load a
  kernel larger than the original boot-sector limit.
- [x] E820-backed physical memory inventory, paging, and separate user address
  spaces.
- [x] Static ELF32 programs running at ring 3, per-process kernel stacks, and
  PIT-driven preemptive scheduling.
- [x] `exit`, `read`, `write`, `open`, `close`, `lseek`, and `getpid` syscalls
  with user-pointer validation and `-errno` error returns.
- [x] ATA primary-master LBA28 PIO polling driver.
- [x] Writable FAT12 for 8.3 files and subdirectories (paths with `.`/`..`):
  create, read, write, truncate, append, mkdir/rmdir, with a VFS and
  per-process file descriptors.
- [x] QEMU headless integration tests covering multi-sector I/O, seeking,
  append, descriptor access checks, spawn/waitpid exit status, exec
  argument/environment stacks, and persisted disk data.

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
  user pages from reserved regions; `USER-MEM-OK`).
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
- [ ] Extend allocation beyond the current identity-mapped range and add
  user virtual-memory allocation and release.
- [ ] Improve page protections and test read-only, unmapped, and
  use-after-free access.
- [ ] Track memory and resource usage per process.

**Acceptance criteria:** A process blocked on I/O does not prevent other
processes from making progress; allocator stress tests complete without frame
leaks or page-table corruption.

### 5. Hardware and User Experience

- [ ] Define a terminal and text-I/O abstraction, separating keyboard input
  from syscalls and display backends.
- [ ] Add framebuffer support only with a boot protocol or safe discovery of
  the framebuffer mode and memory; retain VGA text as a fallback.
- [ ] Improve ATA timeout and recovery behavior and support more devices or
  controllers; consider IRQ/DMA instead of PIO polling if required.
- [ ] Add leveled logging, a panic handler, and useful crash reports in QEMU.

**Acceptance criteria:** The system remains diagnosable without graphics;
display backends use a shared API, and missing framebuffer support does not
prevent boot.

### 6. Developer Tooling and Compatibility

- [ ] Provide a small user-space library and headers for syscalls, strings,
  memory, and file I/O.
- [ ] Add a build target for user C programs and document a stable toolchain
  and ABI.
- [ ] Add automated tests for malformed ELF files, syscalls, VFS, FAT, and the
  scheduler.
- [ ] Define the limited POSIX subset actually supported; add conformance
  tests and document limitations for each capability.
- [ ] Document manual testing and disk-image recovery or replacement.

**Acceptance criteria:** An independent C program can be built and run using
the documented toolchain, and `python build.py headless` runs all regression
tests repeatably.

## Out of Scope for This Roadmap

These features require separate milestones and design decisions:

- SMP, 64-bit/long mode, and architectures other than x86.
- Networking, TCP/IP, USB, audio, and a complete graphical interface.
- Full POSIX/Linux compatibility, a dynamic linker, shared libraries, and
  Linux binaries.
- Journaling or transactional guarantees and power-loss recovery.

## Delivery Process

Keep each milestone small and reversible, update relevant documentation, and
validate changes with the build and headless tests. Move an item to **Current
Status** only after it has a repeatable test and its limitations are documented.
