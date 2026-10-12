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
- [x] Writable FAT12 for 8.3 files in the root directory: create, read, write,
  truncate, and append, with a VFS and per-process file descriptors.
- [x] QEMU headless integration tests with two processes, covering multi-sector
  I/O, seeking, append, descriptor access checks, and persisted disk data.

## Proposed Milestones

### 1. Storage Reliability — Immediate Priority

- [ ] Return distinct, correct errors for FAT12/VFS I/O failures, missing
  files, full disks, and invalid input.
- [ ] Add FAT12 tests for corrupt or cyclic chains, full disks, empty files,
  sector boundaries, and ATA errors.
- [ ] Clean up allocated clusters and metadata as far as possible after failed
  writes, and document that operations are not atomic.
- [ ] Avoid overwriting user data images during every build; separate initial
  image creation from preservation of an existing data image.
- [ ] Provide a way to inspect filesystem images after QEMU shuts down.

**Acceptance criteria:** QEMU success and fault-injection tests demonstrate
that failed operations do not leave leaked descriptors, corrupt FAT chains, or
unexpectedly lost image data across restarts.

### 2. VFS and Filesystem Completion

- [ ] Define a filesystem-independent VFS interface and route descriptor
  operations through it.
- [ ] Add `unlink`, `rename`, `stat`, and `fstat`, including defined behavior
  for open files that are removed or renamed.
- [ ] Add subdirectories and basic paths, including rules for `.` and `..`.
- [ ] Implement and test relevant POSIX errors such as `EEXIST`, `ENOENT`,
  `ENOSPC`, `ENOTDIR`, `EISDIR`, and `EROFS`.
- [ ] Introduce appropriate disk and file locks instead of disabling
  interrupts for the duration of all I/O; add a sector cache if needed.
- [ ] Decide separately whether to support long filenames, partition tables,
  or FAT16; require interoperability tests before expanding scope.

**Acceptance criteria:** Multiple processes can use independent files
concurrently; file and directory operations, EOF, seeking, and errors are
tested, and common FAT tools can still read the disk image.

### 3. Processes, Syscalls, and Program Execution

- [ ] Replace the fixed test-program list with `exec` to load ELF programs
  from disk.
- [ ] Add `wait`/`waitpid`, `fork` or a simpler process-creation model, and
  process exit status management.
- [ ] Define a user-stack convention for arguments and environment variables.
- [ ] Add standard descriptors 0/1/2, descriptor inheritance or transfer, and
  automatic close-on-exit behavior.
- [ ] Add `dup`/`dup2`, limited `brk`/`mmap`, and time syscalls such as
  `gettimeofday`/`nanosleep`.
- [ ] Define a shared user/kernel ABI and headers; test invalid arguments,
  permissions, and device errors for every syscall.

**Acceptance criteria:** A basic shell can launch programs from disk, collect
their output and exit status, and run multiple programs without unintended
address-space or descriptor sharing.

### 4. Concurrency and Memory Management

- [ ] Add kernel synchronization primitives (spinlocks and IRQ-save locks) and
  define locking rules for the scheduler, allocators, VFS, and drivers.
- [ ] Separate wait queues and sleep/wakeup from busy-waiting so blocking I/O
  sleeps the current process instead of spinning.
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
