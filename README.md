<h1 align="center">
 Haku v0.2.0 — a tiny x86 operating system in C
</h1>

## Tech Stack

<p align="center">
  <img src="https://img.shields.io/badge/Language-C-A8B9CC?style=for-the-badge&logo=c&logoColor=white" alt="C" />
  <img src="https://img.shields.io/badge/Assembly-NASM-333333?style=for-the-badge" alt="NASM" />
  <img src="https://img.shields.io/badge/Architecture-x86%20%2F%20i386-0071C5?style=for-the-badge" alt="x86" />
  <img src="https://img.shields.io/badge/Kernel-Bare%20Metal-222222?style=for-the-badge" alt="Bare Metal" />
</p>

<p align="center">
  <img src="https://img.shields.io/badge/Compiler-MSVC%20%2F%20Clang--CL-5C2D91?style=for-the-badge&logo=visualstudio&logoColor=white" alt="MSVC and Clang-CL" />
  <img src="https://img.shields.io/badge/Linker-MS%20LINK%20%2F%20LLD-68217A?style=for-the-badge" alt="Linker" />
  <img src="https://img.shields.io/badge/Build-Python-3776AB?style=for-the-badge&logo=python&logoColor=white" alt="Python" />
  <img src="https://img.shields.io/badge/Assembler-NASM-333333?style=for-the-badge" alt="Assembler" />
</p>

<p align="center">
  <img src="https://img.shields.io/badge/Emulator-QEMU-FF6600?style=for-the-badge&logo=qemu&logoColor=white" alt="QEMU" />
  <img src="https://img.shields.io/badge/Boot-BIOS-555555?style=for-the-badge" alt="BIOS" />
  <img src="https://img.shields.io/badge/Memory-Paging-0078D4?style=for-the-badge" alt="Paging" />
  <img src="https://img.shields.io/badge/Executable-ELF32-6E4C13?style=for-the-badge" alt="ELF32" />
</p>

<p align="center">
  <img src="https://img.shields.io/badge/Filesystem-FAT12-4CAF50?style=for-the-badge" alt="FAT12" />
  <img src="https://img.shields.io/badge/Storage-ATA%20PIO-607D8B?style=for-the-badge" alt="ATA PIO" />
  <img src="https://img.shields.io/badge/Interface-VFS-795548?style=for-the-badge" alt="VFS" />
  <img src="https://img.shields.io/badge/ABI-i386%20Syscalls-455A64?style=for-the-badge" alt="i386 syscall ABI" />
</p>

<p align="center">
  <img src="https://img.shields.io/badge/CI-GitHub%20Actions-2088FF?style=for-the-badge&logo=githubactions&logoColor=white" alt="GitHub Actions" />
  <img src="https://img.shields.io/badge/Platform-Windows%2011-0078D4?style=for-the-badge&logo=windows11&logoColor=white" alt="Windows 11" />
  <img src="https://img.shields.io/badge/Testing-Headless%20QEMU-FF6600?style=for-the-badge" alt="Headless QEMU testing" />
</p>

<p align="center">
  <a href="https://github.com/ParsaAzima/HakuKernel/actions/workflows/ci.yml">
    <img src="https://img.shields.io/github/actions/workflow/status/ParsaAzima/HakuKernel/ci.yml?branch=main&style=for-the-badge&label=CI" alt="CI status" />
  </a>
  <img src="https://img.shields.io/badge/Version-v0.1-blue?style=for-the-badge" alt="Version v0.1" />
  <img src="https://img.shields.io/badge/License-See%20LICENSE-lightgrey?style=for-the-badge" alt="License" />
</p>

---


[![CI](https://github.com/ParsaAzima/HakuKernel/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/ParsaAzima/HakuKernel/actions/workflows/ci.yml)

Bootloader (NASM) loads a C kernel and switches the CPU to 32-bit protected
mode. The kernel initializes paging, interrupts and a GDT/TSS, then reads
user ELF programs from a FAT12 filesystem on an ATA disk and schedules them
at ring 3.

## Prerequisites (Windows 11)

| Tool | Version here | Notes |
|---|---|---|
| Visual Studio 2022 | 17.x, **Desktop development with C++** | we use the x86 `cl.exe` directly — no project/solution needed |
| NASM | 3.02 | `winget install -e --id NASM.NASM` |
| QEMU | 11.x | `C:\Program Files\qemu\qemu-system-i386.exe` |
| Python 3 | 3.14 | drives the whole build (`build.py` + helpers) |

## Build & run

```cmd
cd D:\Source\Haku
python build.py                 :: build haku.img only (data image preserved)
python build.py run             :: build + boot in a QEMU window
python build.py run --fresh     :: recreate the FAT12 data image, then boot
python build.py headless        :: fresh data image + boot headless, verify
python build.py inspect ...     :: examine a FAT12 image (list/check/extract)
```

`python build.py headless` verifies ATA detection on both legacy channels
(`ATA-SCAN-OK devices=2`), the software-reset recovery path
(`ATA-RECOVERY-OK`), cross-channel sector reads (`ATA-MULTI-OK`),
FAT12 mounting and
multi-cluster file reads, subdirectory/path resolution (`USER-DIR-OK`),
ring-3 process entry, per-process kernel stacks,
syscall handling, `spawn`/`waitpid` and exit-status reporting, page-protection
faults (`USER-PROT-OK`), `execve` image replacement, process exit/reaping,
the user C library (`USER-LIB-OK`), and timer IRQs through the serial log.

### Data image lifecycle

`build/haku_fs.img` is the ATA data disk and is treated as user data: an
existing image is reused untouched, so files created in a previous QEMU
session survive the next build. It is (re)created from `kernel/arch/x86/user/user.asm`,
`kernel/arch/x86/user/exec_target.asm`, `kernel/arch/x86/user/child.asm`, the
user library in `user/lib/`, and `user/tests/libtest.c` only when missing or
with `--fresh`. If any test-program source is newer than a reused image,
the build warns that its test ELFs on disk may be stale. `headless` always
starts from a fresh image (its fault-injection tests mutate the disk) and,
after the serial-log check passes, re-verifies the post-run image on the
host side.

`python build.py inspect` (default: `build/haku_fs.img`) lists the root
directory and FAT summary without touching the image; `--chains` also prints
cluster chains, `--cat NAME.EXT` / `--dump NAME.EXT OUT.BIN` extract a file,
and `--check` enforces the kernel's mount invariants (BPB sanity, mirrored
FATs, exact chains, no orphans) with a nonzero exit on any damage.
The build is one Python script on purpose — `cmd.exe` quoting of spaced
paths (e.g. `C:\Program Files\...`) is fragile, while `subprocess` argument
lists just work.

### Continuous integration and releases

`.github/workflows/ci.yml` runs on every push to `main`, every pull request,
and manual dispatch. It uses `ubuntu-latest`, installs NASM, Clang/LLD, and
QEMU from apt, runs `python build.py headless` through the
`clang-cl`/`lld-link` path in `build.py`, and uploads `haku.img`, the data
image, serial/QEMU logs, and any crash screenshots as artifacts even when the
boot check fails. A missing marker, an unexpected `*-FAIL` marker, or
`KERNEL PANIC` fails the job.

`.github/workflows/release.yml` runs for `v*` tags. It repeats the headless
check, deletes the mutated test image, rebuilds a pristine
`build/haku_fs.img`, and publishes `Haku-<tag>.zip` containing `haku.img`,
`haku_fs.img`, `README.md`, `LICENSE`, and `SHA256SUMS.txt`:

```bash
git tag v0.2.0
git push origin v0.2.0
```

Slow runners can tune headless timing with `HAKU_BOOT_DELAY` (seconds before
the injected key, default 1) and `HAKU_HEADLESS_TIMEOUT` (seconds to wait for
guest shutdown, default 30).

## How it works

```
BIOS (0x7C00)
  └─ kernel/arch/x86/boot/{stage1,stage2}.asm (16-bit): load kernel at 0x10000
      → A20 (Linux-style: test after BIOS/KBC/fast methods) → GDT → CR0.PE=1
         └─ far jump to KERNEL_ENTRY (= PE entry point from link.exe)
               └─ kernel/arch/x86/kernel/entry.asm (_entry stub) → call kernel_main
                     └─ kernel/kernel/kernel_main.c: paging/IDT → GDT+TSS → scheduler
                          └─ timer-preempted ring-3 processes → int 0x80
```

Memory map:

| Address | Use |
|---|---|
| `0x7C00` | boot sector (512 B) |
| `0x10000–0x80000` | kernel load window (up to 448 KiB, linked `/BASE:0x10000 /ALIGN:16`) |
| `0x7000–0x7240` | E820 map from stage2 (INT 15h, count at `0x6FFC`) |
| `0x9000–0xC000` | page directory + initial tables (8 MiB supervisor identity map) |
| `0xF0000000–0xF8000000` | supervisor direct map of physical `0–128 MiB` (page tables live in kernel `.bss`) |
| `0x100000–0x400000` | kernel heap (kmalloc/kfree, 16 B aligned) |
| `0x90000` | bootstrap kernel stack top (grows down) |
| low E820 frames (often 4–8 MiB) | per-process 8 KiB kernel stacks, accessed through the direct map |
| `0x40000000` | user program code/data virtual base (per-process mapping) |
| `0x50000000–0x54000000` | user `brk` heap region (lazily mapped) |
| `0x60000000–0x70000000` | anonymous `mmap` region (lazily mapped) |
| `0xBFFFE000–0xC0000000` | two user stack pages |
| `0xB8000` | VGA text buffer |

### Interrupts (IDT)

`kernel/arch/x86/kernel/isr.asm` builds a uniform stack frame for all 256
vectors (dummy error code where the CPU pushes none), saves all data segments,
and calls into C. `kernel/arch/x86/kernel/idt.c`
installs 32 exception gates (0–31), 16 IRQ gates (32–47, used once the
8259 is remapped) and a catch-all for 48–255 that reports instead of
triple-faulting. The syscall gate at vector 128 is DPL 3; all other gates
remain ring-0-only. All are interrupt gates, so handlers run with IF clear.
The boot proves the exception path with an `INT3` self-test.

### Physical frame allocator

`kernel/arch/x86/mm/frame.c` now builds its usable-frame bitmap from the 64-bit base and
length fields in the E820 entries. Only complete 4 KiB pages wholly inside
type-1 usable ranges are admitted; invalid entries and overlapping reserved
ranges are excluded. The allocator explicitly reserves low boot/firmware
memory, the kernel's 448 KiB load window, page tables, the boot stack, the ISA
hole, and the 1–4 MiB heap. `frame_usable_count()` reports E820-usable pages
tracked up to 128 MiB, and every tracked frame is allocatable: a permanent
supervisor direct map at `0xF0000000` addresses physical `0–128 MiB`, and its
PDEs are copied into each user page directory. The self-test chain-allocates
past the old 8 MiB identity boundary and writes/reads a high frame through
`PHYS_TO_VIRT`. Freeing validates alignment, usability, and allocation state
so reserved pages cannot accidentally be released.

### Processes and preemptive scheduling

`kernel/kernel/scheduler.c` starts the initial process from `HAKU.ELF`; a successful
`spawn` creates a child process from a root-filesystem ELF, `fork` creates one
as a copy of the caller, and `waitpid`/`wait4` suspends the parent until the
requested child exits. Exit status is returned in the traditional high-byte
wait status format; a successful wait reaps the child record. The current
spawn interface accepts a pathname only and starts the child with empty
argument and environment vectors; the child inherits a copy of the parent's
descriptor table (standard descriptors and any open files, each sharing the
parent's open file description). `waitpid` accepts a specific child PID or
`-1` (any child) and currently supports only options `0`; `wait4` follows the
Linux call, accepting `WNOHANG` and writing a zeroed `struct rusage` (the
per-process accounting described below is not yet wired into it).
`exit_group` ends the calling process like `exit`. `clone` is accepted only
for the `SIGCHLD` form (`fork` with no sharing flags) and returns `EINVAL` for
the thread-creating flags, stack pointer, or parent-tid arguments, so no
half-supported threading model is exposed. A forked child gets a private copy
of the address space (code, data, stacks, heap, and mappings), a copy of the
caller's saved register frame with `eax` zeroed, and its own descriptor table;
the parent sees the child PID. Fork does not yet share or copy expanded
segments lazily (copy-on-write), so the copy is eager. `execve` replaces a
process image from the root filesystem.
Each process has its own address space and an 8 KiB ring-0 stack; the stack
is allocated as physical frames and addressed through the kernel direct map.
`gdt_set_kernel_stack` updates `TSS.esp0` at every task switch so the next
ring-3 interrupt or syscall enters on the correct stack. The 100 Hz PIT
scheduler saves and restores complete interrupt frames through `iret`.
User exceptions and `exit` mark a process for reaping; its address space and
kernel stack are reclaimed after execution has moved to another stack. When
no user process is runnable, the scheduler runs a dedicated kernel idle
context.

`kernel/arch/x86/kernel/gdt.c` installs flat ring-0/ring-3 code and data descriptors plus a
32-bit TSS. `kernel/arch/x86/mm/paging.c` gives each process supervisor-only kernel
identity and direct-map entries and maps its user program and two-page user
stack with user permissions.

`kernel/arch/x86/user/user.asm` is built as an ELF32 image and entered via `iret` at CPL 3.
It tests `spawn` (`0x1000`, Haku-specific), `fork` (2), `wait4` (114), and
`waitpid` (7), as well as
`execve` (11), which loads another ELF32 image, builds an aligned
initial user stack containing `argc`, `argv`, `envp`, and a terminating
`AT_NULL` auxiliary-vector entry, and replaces the calling process without
changing its PID or open file descriptors. Each vector is limited to 64
strings; their strings are limited to 4096 combined bytes including NUL
terminators and must fit the two-page initial stack. Larger inputs return
`E2BIG`, and invalid user pointers return `EFAULT`. The syscall ABI follows
the i386 Linux syscall numbers for a small POSIX-oriented subset: `exit` (1),
`fork` (2), `read` (3), `write` (4), `open` (5), `close` (6), `waitpid` (7),
`unlink` (10), `execve` (11), `lseek` (19), `getpid` (20), `rename` (38),
`mkdir` (39), `rmdir` (40), `dup` (41), `brk` (45), `dup2` (63),
`gettimeofday` (78), `mmap` (90), `munmap` (91), `stat` (106), `lstat` (107),
`fstat` (108), `wait4` (114), `clone` (120), `nanosleep` (162),
`stat64` (195), `lstat64` (196),
`fstat64` (197), `exit_group` (252), `fstatat64` (300), `uname` (122),
`getrusage` (77), `times` (43), `getppid` (64), `getuid` (24), `geteuid` (49),
`getgid` (47), `getegid` (50), and `statx` (383), with arguments in
`ebx`, `ecx`, `edx`, `esi`, and `edi` and syscall number/result in `eax`.
The Haku-specific `spawn` and `getusage` syscalls use `0x1000` and `0x1001`.
The public ABI definitions are in `include/haku/abi.h` for C and
`include/haku/abi.inc` for NASM; the build checks that their numeric
constants stay synchronized. They define the syscall numbers (the ones
above plus the Linux numbers the coming milestones add, which answer
`ENOSYS` until then), errno values, open flags, and the metadata layouts.
`haku_stat_t` is the 64-byte i386 Linux `struct stat` returned by `stat`,
`lstat`, and `fstat`; `haku_stat64_t` is the 96-byte i386 `struct stat64`
returned by `stat64`, `lstat64`, `fstat64`, and `fstatat64` — the layout
glibc and musl use on i386 — and `haku_statx_t` is the 256-byte `struct
statx` returned by `statx`, the call musl makes for `stat()` on i386.
Regular files report mode `0644` (`S_IFREG`), directories `0755`
(`S_IFDIR`), and the metadata carries the link count, owner, size, 512-byte
block size, and block count.
`haku_timeval_t` (`tv_sec`, `tv_usec`) and `haku_timespec_t`
(`tv_sec`, `tv_nsec`) are the 8-byte user time structures. `haku_mmap_arg_t`
is the 24-byte i386 `old_mmap` argument block.

`brk` grows or shrinks a per-process heap starting at a reserved virtual base
(`0x50000000`): the break is page-rounded and pages are mapped zero-filled as
it rises; lowering it unmaps the released pages and returns their frames; a
below-base break gives `EINVAL` and one past the heap limit gives `ENOMEM`.
`mmap` takes a pointer to an `old_mmap` argument block in `ebx` and supports
only anonymous private mappings (`MAP_PRIVATE | MAP_ANONYMOUS`) with a zero
address hint, no execute bit, and offset 0; it places mappings in a second
reserved region (`0x60000000`) using first-fit over a per-process table, so
earlier releases are reused. `munmap` (91) releases a range that lies inside
one tracked anonymous mapping, trims or splits the region, and frees the pages
(and any now-empty page tables). `PROT_WRITE` selects the PTE `R/W` bit, so a
`PROT_READ` mapping is enforced read-only by the MMU; `PROT_NONE` reservations
are rejected with `EINVAL`. Both regions are reclaimed with the rest of the
address space at exit or `execve`.

The kernel sets `CR0.WP`, so ring-0 writes also honour `R/W=0`: read-only user
pages can only be filled through their physical direct-map alias. ELF segments
are mapped from their `PF_W` bit (the loader already rejects `W|X`), and
`munmap` clears the PTE before returning the frame, so both an `RX` store and a
store through a released mapping take a page fault. i386 without PAE has no
execute-disable bit, so W^X is enforced by the loader's `W|X` rejection rather
than by an NX permission.

### Per-process memory and resource accounting

`paging_map_user_page` and `paging_unmap_user_page` maintain per-address-space
counters for present user pages, non-directory user page tables, and a
mapped-page high-water mark. ELF loading, `brk`, `mmap`, and `munmap` all go
through those helpers, so the counters follow every mapping change without a
table walk. The scheduler attributes each 100 Hz timer tick to the interrupted
context by its CS privilege level, counts syscalls at the `int 0x80` entry,
and records a voluntary switch when a task blocks, sleeps, or waits and an
involuntary switch when the timer moves a runnable task aside. `execve`
preserves the process's peak and resource totals while replacing the current
image's memory counters with those of the new address space.

`getusage` (0x1001, Haku-specific) copies a 64-byte `haku_usage_t` to a
validated user buffer. It reports current and peak mapped pages, page-table
and kernel-stack pages, current `brk` and anonymous `mmap` bytes, and derived
resident/kernel/total byte counts, along with the pid, user/kernel timer
ticks, syscall count, and voluntary/involuntary context switches.
`resident_bytes` covers mapped user pages; `kernel_bytes` covers the user page
tables, the page directory, and the 8 KiB kernel stack; `total_bytes` is their
sum. A NULL or unmapped buffer returns `EFAULT`. Accounting is per process,
not per thread (the kernel has no threads), there is no demand paging, and
ticks have 10 ms granularity.

### User-mode system calls

`read` supports stdin from the PS/2 keyboard and regular files; `write`
supports stdout/stderr to the active display and serial as well as writable
regular files. The standard descriptors `0`/`1`/`2` are real per-process
descriptors backed by a registered console device rather than special cases
in the syscall layer: they resolve through the same VFS descriptor table as
file handles, and the device itself only forwards to the terminal layer.
`open`, `close`, `lseek`, `unlink`, `rename`, `mkdir`, `rmdir`, `stat`,
`fstat`, and per-process file descriptors are backed by the VFS/FAT12 layer.
Paths are `/`-separated 8.3 names resolved from the root; `.` keeps the
current directory and `..` climbs to the parent (clamped at the root). `stat`
and `fstat` report byte size and mode (`S_IFREG | 0644` for files,
`S_IFDIR | 0755` for directories) through an 8-byte user buffer. Reads
and writes are bounded to 4096 bytes, and errors return
negative errno values (the user library in `user/lib` translates these to
`errno`).
Every FAT12 failure class keeps one distinct errno end to end: missing
files give `ENOENT`, bad flags/whence/pointers give `EINVAL`, existing
files give `EEXIST`, bad descriptors give `EBADF`, a full volume gives
`ENOSPC`, a non-directory path component gives `ENOTDIR`, opening or
unlinking a directory gives `EISDIR`, removing a non-empty directory gives
`ENOTEMPTY`, and disk I/O failures give `EIO`. A root mounted with
`VFS_MOUNT_RDONLY` rejects create, write, truncate, unlink, rename, mkdir,
and rmdir with `EROFS` while reads and metadata keep working. After the
user process exits,
the idle task runs an error self-test (`FS-ERRORS-OK`) including a
root-fill probe that must fail with `ENOSPC` (`FS-ENOSPC-OK`). The same
pass also remounts read-only, checks every mutating path returns `EROFS`,
remounts read-write (`FS-EROFS-OK`), covers empty files, exact-sector and
boundary-spanning I/O
(`FS-FILE-EDGE-OK`), injected cut/bad/reserved/cyclic FAT chains (each
restored, reads must give `EIO`, loops must terminate) plus a crafted
full data area (`FS-CORRUPT-OK`), and ATA bounds/NULL-transfer rejection
(`FS-ATA-OK`). The same pass checks the metadata behind `stat`/`fstat`:
type bits, size, link count, block size, and block count for a file and a
directory (`FS-STAT-OK`).

`gettimeofday` reports whole seconds and microseconds since boot derived
from the 100 Hz PIT tick counter; a NULL timeval is allowed (a no-op query),
an unmapped one gives `EFAULT`, and the unused timezone argument is ignored.
`nanosleep` rounds the requested interval up to the next timer tick, marks
the calling task `TASK_SLEEPING`, switches away, and lets the timer IRQ wake
it when its deadline passes, so it does not busy-wait. A zero interval
succeeds immediately; `tv_nsec` outside `[0, 1e9)` gives `EINVAL` and a NULL
request gives `EFAULT`. There are no signals, so a sleep is never
interrupted. The number of sleeping tasks is not counted against idle, so a
sleeping process keeps the idle loop in the scheduler.

Blocking events use wait queues (`kernel/kernel/scheduler.h`). A queue is a
bitmap over scheduler task slots, so it needs no allocation and cannot fail.
`wait_queue_sleep` marks the caller `TASK_BLOCKED` with interrupts still
disabled; the caller then executes `sti; hlt`, so either the event IRQ returns
straight to the halted task or the timer IRQ saves that frame and switches to
another task. A blocked task is off the run queue, so when nothing else is
runnable the scheduler selects the idle context instead of rescheduling it.
`wait_queue_wake_all` is called from IRQ context and marks every parked task
runnable; waiters re-check their condition after resuming. Keyboard input is
the first user of this: IRQ1 pushes the character into the terminal's input
ring and wakes its readers, and `SCHED-WAITQ-OK` proves that a read waiting
for a key left the run queue and let idle run. `nanosleep` and `waitpid` keep
their existing `TASK_SLEEPING`/`TASK_WAITING` states; the blocked state is
deliberately separate so a child exiting cannot wake a task that is waiting
for console input.

Writes are not atomic: there is no journal, so a failing multi-sector
write leaves exactly its durable prefix (a short count means those bytes
are on disk and the size covers them), while a total failure restores
the pre-call size and releases clusters the call allocated
(`FS-ROLLBACK-OK`). Bytes are never attempted past a gap that failed to
zero, so no unwritten holes are ever claimed. If persisting the directory
entry itself fails, the call rolls back to the pre-call metadata rather
than leaking unreachable clusters; a TRUNC whose chain cannot be freed
zeroes the entry best-effort so the file reads back empty instead of
pointing at a broken chain. Anything beyond that best effort (for
example, clusters orphaned by a dying disk) stays leaked: there is no
fsck yet.

Open files survive removal and renaming: `unlink` deletes the name at
once while open descriptors keep reading, writing, and seeking the data
(clusters are released at the last such close); `rename` moves the entry
and repoints open descriptors to it, so they keep working under the new
name. Renaming onto an existing name fails with `EEXIST`, renaming a name
onto itself succeeds as a no-op, and every other missing name gives
`ENOENT`. The user test program proves all of this from ring 3 in both
processes (`USER-FILE-OPS-OK`).

`dup` and `dup2` point a second descriptor at the same open file
description: the copy shares the byte offset and access mode, and the file
stays open until the last descriptor closes, so closing the original leaves
the duplicate usable (and an open-on-a-removed-file survives to its final
close). `dup` returns the lowest free descriptor, `dup2` closes and reuses a
free or already-open target and is a no-op when both descriptors match, and
both report `EBADF` for an invalid source without disturbing the target. The
user program proves the shared offset, the surviving duplicate, and the
error cases in both processes (`USER-DUP-OK`).

Because `0`/`1`/`2` are ordinary descriptors, `dup2` can redirect stdout to a
file (a shell-style redirect) and `close` revokes a standard descriptor so
later I/O returns `EBADF` until `dup2` restores it. `spawn` copies the
parent's descriptors into the child, and every exit path releases the exiting
process's descriptors, so files a child leaves open are closed automatically
when it terminates. The user program and its child prove the redirect,
revoke, inherited-descriptor, and close-on-exit paths (`USER-STDIO-OK`,
`USER-FD-INHERIT-OK`); after both processes exit, the idle self-test asserts
their descriptor tables are empty.

Before copying, the kernel validates every covered page in the current page
tables for presence, user access, and write permission where required. It
rejects unmapped/kernel pointers, wraparound, invalid descriptors and
oversized I/O. The syscall entry enables interrupts while in kernel code so
blocking keyboard reads can make progress; the keyboard wait is atomic
against the interrupt/wakeup race. Ring-3 page faults are contained and
reported rather than halting the kernel: the fault handler prints a crash
report (vector, error code, `CR2`, control registers, general registers, and
the validated user stack), encodes the error code in the exit status byte
(`0x80 | err`), and terminates only the faulting process. These syscalls are an
initial POSIX-inspired kernel interface, not full POSIX conformance. Run
`python build.py headless` to test read/write/open/close/lseek/getpid/exit,
gettimeofday/nanosleep, brk/mmap/munmap, per-process `getusage` memory and
resource accounting, bad pointers, descriptor errors, input copying,
disk-backed create/write/read across a sector boundary, and continued
timer operation. The protection test spawns one child per fault
(`USER-PROT-OK`): a store into an `RX` segment, a store into a `PROT_READ`
mapping, a read from unmapped memory, and a store through a `munmap`ed page,
each checked against its expected `#PF` error code (`7`, `7`, `4`, `6`). The
blocking read is also observed to park the process (`SCHED-WAITQ-OK`) before
the injected key arrives.

### User C library

`include/haku/` doubles as a freestanding C SDK: `<haku/haku.h>` includes the
syscall ABI, `types.h`, `errno.h`, `string.h`, `stdlib.h`, `unistd.h`,
`fcntl.h`, `stat.h`, and `stdio.h`.

- Raw syscalls: `haku_syscall0`..`haku_syscall4` in `user/lib/syscall.asm`
  (i386 `int 0x80`, arguments in `ebx`/`ecx`/`edx`/`esi`, result in `eax`).
  They return the kernel's `-errno` unchanged.
- POSIX-style wrappers return `-1` and set `errno`: `read`, `write`, `close`,
  `lseek`, `dup`, `dup2`, `open`, `stat`, `lstat`, `fstat`, `unlink`,
  `rename`, `mkdir`, `rmdir`, `getpid`, `waitpid`, `execve`, `spawn`,
  `getusage`, `gettimeofday`, `nanosleep`, `sleep`, `brk`, and `sbrk`.
  `stat`/`lstat` use the Linux `fstatat64` (300) call with `AT_FDCWD` and
  `fstat` uses `fstat64` (197), so the library and a musl program read the
  same 96-byte `struct stat64` layout.
- Strings and memory: `mem*`, `str*`, `atoi`, `malloc`/`calloc`/`realloc`/
  `free` (a first-fit free list over `brk`), and `printf`/`puts`/`putchar`
  with a `%c/%s/%d/%u/%x/%X/%p` subset (no floats, 512-byte output buffer).
- `user/lib/crt0.asm` supplies `_start`: it calls `main(void)` and exits with
  the returned status.

`build.py` compiles the library and `user/tests/libtest.c` with the same
`cl.exe` (or `clang-cl`) used for the kernel, links a fixed-base PE32 at
`0x40000000` with no CRT, and `tools/pe2elf.py` repacks its RX/R/RW sections
into ELF32 `PT_LOAD` segments. The image is installed as `LIB.ELF`;
`NEXT.ELF` spawns it, requires a clean exit status, and the run prints
`USER-LIB-OK`. There is no C runtime, dynamic linker, standard header set,
floating-point formatting, or thread-safe allocator, and data-image filenames
remain 8.3. The user ELFs are regenerated on every build, but an existing
`build/haku_fs.img` only receives `LIB.ELF` after `--fresh` or `headless`.

### Terminal, display, and input

`kernel/drivers/terminal.c` is the text-I/O layer. Output paths (`klog`, the
exception handlers, and the console device) call `terminal_putc` /
`terminal_write`, which render to the active display backend and mirror every
byte to COM1 with CRLF line endings. Input is a 128-byte single-producer /
single-consumer ring owned by the terminal: `kbd.c` translates set-1 scancodes
and calls `terminal_input_push`, while `terminal_getchar`/`terminal_read` park
on the terminal's wait queue until IRQ1 wakes them. Neither the keyboard
driver nor the syscall layer knows about the other.

Display backends implement the shared `display_ops_t` table in
`kernel/drivers/display.h` (`name`, `clear`, `putc`, `status`). The VGA text
backend is selected at boot and stays the fallback; when the bootloader
hands over a usable VBE mode, `framebuffer_init` succeeds and the boot path
switches the active backend with `display_use(&framebuffer_display)`. Backend
state is protected with IRQ-save spinlocks because the timer redraws the
status line from interrupt context.

### Logging, panic, and crash reports

`kernel/kernel/klog.c` provides five severities (`DEBUG`, `INFO`, `WARN`,
`ERROR`, `CRIT`) with an `INFO` default threshold. `klog_log` and
`klog_begin` add the tag and honour `klog_set_level`; the plain
`klog_write`/`klog_prefix` path stays unconditional for boot markers and
test output. `klog_self_test` proves that `DEBUG` is suppressed at the
default threshold and appears after lowering it (`KLOG-LEVELS-OK`).

`kernel/kernel/panic.c` is the fatal path. It disables interrupts, prints
`KERNEL PANIC: <reason>`, and calls `crash_report` (`kernel/kernel/crash.c`),
which dumps `CR0`/`CR2`/`CR3`, the current pid, the exception frame
(`vector`, `err`, `eip`, `cs`, `eflags`), the general registers, segment
registers, and the interrupted stack. Ring-0 stacks are dumped directly;
ring-3 stacks are dumped only after `paging_user_range_valid` confirms they
are mapped, so a bad user stack cannot fault the reporter. The CPU is then
parked in `cli; hlt`. Boot self-test failures (`PAGING`, `HEAP`, `FRAME`,
`GDT`, `usermode`) and fatal kernel exceptions use the same handler.

Contained ring-3 faults use the same report but terminate only the faulting
process, and the first one logs `CRASH-REPORT-OK`. QEMU captures the report
in `build/serial.log`; `build.py` also passes `-d int,cpu_reset` and writes
`build/qemu.log`, and a headless run fails if `KERNEL PANIC` appears. To
exercise the panic path without changing code, boot with a QEMU monitor and
send `nmi` while the idle loop is running: the serial log then shows
`KERNEL PANIC: NMI` followed by the `PANIC:` register and stack dump.

### ATA PIO and FAT12

`kernel/drivers/ata.c` probes the legacy primary and secondary ATA channels
(`0x1F0`/`0x3F6` and `0x170`/`0x376`) at both master and slave positions,
ignores floating buses and ATAPI devices, and keeps every usable disk in a
device list. Devices are identified with IDENTIFY; LBA28 is used when
available and LBA48 addressing is the fallback. Identify, read, write, and
cache-flush commands use bounded polling PIO: a failed or timed-out command
is retried after a software reset (SRST) up to a fixed number of attempts,
and every write is followed by FLUSH CACHE before it is reported complete
(a drive that rejects the flush keeps the accepted data and only logs a
warning). PIO polling is retained deliberately: the initial FAT mount runs
before the PIC and PIT are initialized, while IRQ completion requires
remapping/unmasking IRQ14/15 and DMA completion adds PCI bus-master
programming and physical-buffer constraints. The synchronous path keeps boot
deterministic and headless-testable; AHCI, PCI native-mode controllers, and
ATAPI are not supported. The boot self-tests prove the recovery path
(`ATA-RECOVERY-OK`: a software reset followed by an identical sector reread)
and cross-channel I/O (`ATA-MULTI-OK`: the second device answers a sector
read), and the headless run checks the scan itself (`ATA-SCAN-OK devices=2`).
`kernel/fs/fat12/fat12.c` mounts a
FAT12 superfloppy from any detected ATA disk (the first disk that carries a
valid BPB wins, so the volume may live on the secondary channel or on a
slave) and supports creating, reading, writing, truncating and
seeking 8.3-named files anywhere in the tree, plus `mkdir`/`rmdir` of
single-cluster subdirectories with real `.`/`..` entries. Paths are
`/`-separated and resolved from the root; `.` and `..` are handled during
resolution. Cluster allocation, release, mirrored FAT updates and
volume/chain bounds checks are shared by files and directories.
`kernel/fs/vfs/vfs.c` supplies per-process descriptors and the user `open`, `close`,
`read`, `write`, and `lseek` interface through a filesystem-independent
operations vector (`vfs_fs_ops`: mount/open/close/read/write/seek/unlink/
rename/mkdir/rmdir/stat/fstat, each returning 1 or a `-VFS_E*` errno). VFS
never names a concrete filesystem;
`kernel/fs/fat12/fat12_vfs.c` implements the vector for FAT12 (flag translation,
per-open handle allocation, errno mapping) and registers it at boot, after
which one backend is mounted as the root filesystem. A root can be mounted
read-only (`vfs_mount_root_flags(..., VFS_MOUNT_RDONLY)`); VFS then returns
`EROFS` for every mutating operation instead of calling the backend. Disk
I/O is serialized
against the preemptive scheduler; file contents and directory/FAT metadata
are written through to the ATA disk. The current VFS supports up to five
simultaneously open regular files per process (descriptors 3-7; `0`/`1`/`2`
are the console) and the `O_RDONLY`, `O_WRONLY`, `O_RDWR`, `O_CREAT`,
`O_EXCL`, `O_TRUNC`, and `O_APPEND` flags.

The filesystem remains intentionally small: no partition tables, long
filenames, permissions, mount points, journaling, FAT16, or FAT32.
Directories are one cluster (16 entries) and do not grow; moving a directory
across parents with `rename` leaves its `..` entry pointing at the old
parent, so directories should be renamed in place. This is a minimal
single-volume VFS, not full POSIX filesystem conformance. The QEMU headless
test creates `P1.TXT`, exercises `DIR1/T.TXT` with `./DIR1/../DIR1` paths,
and verifies multi-sector writes, reads, seeks, close, and reopen.

`tools/mkfat12.py` puts `HAKU.ELF`, the exec target `NEXT.ELF`, and the
spawn/wait child `CHILD.ELF` on a generated 1.44 MB FAT12 image. QEMU
attaches this image as the primary-master IDE disk while the existing boot
floppy still starts the kernel; the build also attaches a generated
1.44 MB spare disk as secondary master so the headless run exercises
device scanning across both legacy channels. The kernel
loads the initial ELF at startup; `execve` uses VFS to load subsequent ELF
files from the disk into a fresh address space before replacing the current
image.

### Locking

`kernel/kernel/sync.h` provides three primitives on top of `cpu_irq_save`/`restore`:

- `irq_lock`/`irq_unlock` — a plain IRQ-save pair for short critical sections
  that never block and never call into a blocking path. Nests correctly.
- `spin_lock`/`spin_unlock` — a task-context spinlock. The critical section
  runs with interrupts enabled, so a preempted holder can be scheduled again
  while waiters busy-wait. Must never be acquired with interrupts disabled
  and never held across a blocking call.
- `spin_lock_irqsave`/`spin_unlock_irqrestore` — a spinlock whose holder also
  disables interrupts, making it non-preemptible and safe from IRQ context.

The locking rules per subsystem:

- **Scheduler**: every task-array mutation runs with interrupts disabled (in an
  IRQ handler or under `irq_lock`), so no spinlock is needed. A task-context
  spinlock must never be taken with interrupts disabled — a preempted holder
  would never run again to release it. `scheduler_spawn` therefore reserves a
  task slot, then re-enables interrupts for `vfs_inherit` before publishing the
  task as runnable.
- **Allocators** (`kernel/arch/x86/mm/frame.c`, `kernel/kernel/heap.c`): IRQ-save
  spinlocks. The free-list/bitmap updates are short and the holder cannot be
  preempted, so the allocators are safe from both task and interrupt context.
- **VFS** (`kernel/fs/vfs/vfs.c`): one task-context spinlock, `vfs_lock`, around all
  descriptor/OFD table changes and file-backend calls. Interrupts stay enabled
  during disk I/O, so a preempted holder can finish; console device I/O drops
  the lock first because it may block. `ofd_release` does not lock — callers
  hold `vfs_lock`.
- **Drivers**: `kernel/drivers/ata.c` uses a task-context spinlock around the polling
  transfers; `kernel/arch/x86/kernel/kbd.c` only translates scancodes and
  forwards them to `terminal.c`, which owns the input ring (single-producer
  IRQ1, single-consumer task) and the IRQ-safe wait queue; `terminal.c`
  serializes output through the display and UART backends, which use IRQ-save
  spinlocks because `klog` may run from interrupt context.

Locks are non-recursive; the only ordering rule is VFS → backend, never the
reverse.

### ELF32 program loading

`kernel/arch/x86/elf/elf.c` loads static ELF32 little-endian i386 `ET_EXEC` images read
from FAT12. It validates the ELF header and program-header bounds, accepts
non-overlapping readable `PT_LOAD` segments, checks address arithmetic and
file ranges, and rejects dynamic/interpreted images and writable-executable
segments. Each segment is mapped according to its ELF permissions; the loader
zeroes `p_memsz - p_filesz` for BSS and creates a two-page user stack.

Each loaded image receives a fresh page directory containing supervisor-only
kernel identity/direct mappings plus only that image's user pages. The scheduler
activates each process's CR3 on dispatch and returns to the kernel page
directory for its idle context. Exited process mappings and page tables are
released after switching away from their kernel stacks. Dynamic linking,
relocations, shared libraries, and ELF files larger than the current 256 KiB
loader buffer are not supported.

### Why `link.exe` + `tools/pe2bin.py`?

MS `link.exe` cannot emit flat binaries and raw COFF objects carry
MSVC-specific relocations, so we let the **real linker** do its job:
`entry.obj + arch/x86/kernel/isr + arch/x86/user objects + kernel C objects` → fixed-base PE
(`/BASE:0x10000 /ENTRY:entry`),
then the Python script only copies section bytes to their RVA — zero
relocation math on our side, zero MSVC quirks to chase. The reported
`ENTRY=` address and exact padded kernel sector count are passed to stage 2.
The stage-2 CHS loop reads track-bounded chunks into the 448 KiB load window,
leaving 64 KiB below it for the bootstrap stack; `mkimg.py` enforces both
the memory and floppy-image limits. A chunk is also clamped so an INT 13h
transfer never crosses a 64 KiB DMA boundary; when that clamp shortens a
read, the loop advances the sector within the current track instead of
skipping to the next head, which keeps a large kernel image contiguous.

The A20 routine mirrors Linux `arch/x86/boot/a20.c` (test → BIOS →
test → keyboard controller → long test → fast gate, with the KBC drain
loop), and the COM1 init mirrors Linux `early_serial_init`.

## Project layout

```
Haku/
  kernel/
    arch/x86/
      boot/             stage1.asm + stage2.asm: 16-bit boot sector and CHS
                        kernel loader (up to 448 KiB) → A20 → GDT → protected mode
      kernel/           entry.asm + isr.asm: 32-bit entry, interrupt stubs,
                        GDT/TSS load, ring-3 transition + syscalls
                        cpu/gdt/idt/pic/pit/kbd: port I/O, descriptor tables,
                        PIC/PIT, and the PS/2 keyboard driver
      mm/               paging.c/.h: i386 page tables (identity + 128 MiB direct map + user)
                        frame.c/.h: E820-backed physical page inventory
      elf/              elf.c/.h: validated ELF32 PT_LOAD loader + user spaces
      user/             user.asm, child.asm, fault.asm, exec_target.asm: ELF32
                        test programs (syscall, spawn/waitpid, protection
                        faults, execve coverage)
    drivers/            ata.c/.h: primary/secondary ATA (master/slave) LBA28/LBA48
                        polling PIO with reset/retry recovery and cache flush
                        terminal.c/.h: text-I/O layer (output + input ring)
                        display.c/.h: shared display backend API
                        vga_text.c/.h: VGA text backend (fallback)
                        framebuffer.c/.h: VBE framebuffer text backend
                        serial.c/.h: COM1 output backend
    fs/
      fat12/            fat12.c/.h: writable FAT12 files, subdirectories, paths
                        fat12_vfs.c/.h: FAT12 vfs_fs_ops backend + errno mapping
      vfs/              vfs.c/.h: fs-independent descriptors, registry, root mount
      test/             fs_test.c/.h: kernel-side filesystem self-tests
    kernel/             kernel_main.c: kernel startup
                        scheduler.c/.h: scheduler + per-process usage counters
                        sync.c/.h: spinlocks and IRQ-save locks + locking rules
                        usermode.c/.h: user mappings, syscall ABI, brk/mmap/munmap
                        getusage
                        klog.c/.h: timestamped, leveled terminal logging
                        panic.c/.h: fatal handler + halted state message
                        crash.c/.h: register/MMU/stack crash reports
                        heap.c/.h: kernel kmalloc/kfree allocator
  user/
    lib/              errno.c, memory.c, stdio.c, string.c, unistd.c,
                      crt0.asm, syscall.asm: user C library
    tests/            libtest.c: C library self-test installed as LIB.ELF
  include/haku/         abi.h + abi.inc: shared user/kernel ABI constants
                        types/errno/string/stdlib/unistd/fcntl/stat/stdio.h:
                        user library headers, plus haku.h
  tools/pe2bin.py      PE image → flat binary (+ reports ENTRY= for the bootloader)
  tools/mkimg.py       boot floppy size/signature checks
  tools/mkfat12.py     FAT12 image builder for the user ELF
  tools/pe2elf.py      fixed-base PE32 to ELF32 (RX/R/RW) for user C
  .github/workflows/  ci.yml: hosted headless build/test + artifacts
                      release.yml: tag-driven bootable image release
  build.py             the whole build: cl/link/nasm/qemu orchestration
  build/               generated files (stage*.bin, *.obj/pe, haku.img, ...)
  haku.img             bootable floppy image
  build/haku_fs.img    1.44 MB FAT12 ATA disk image with HAKU/NEXT/LIB ELFs
  build/ata_spare.img  1.44 MB blank ATA disk on the secondary channel (scan test)
```

## Troubleshooting

- **`not a 32-bit image`** — you used the x64 `cl.exe`; build.py picks
  `…\bin\Hostx64\x86\cl.exe` automatically, don't override it.
- **unresolved external (e.g. `_memset`)** — a CRT helper leaked in; add it
  to `kernel.c` (or simplify the code that triggered it).
- **NASM win32 drops all `global` symbols without `section .text`** — code
  assembles fine but the .obj has no definitions (found by dumping symbols).
  Every NASM-for-link file needs an explicit section directive.
- **MSVC prepends `_`, NASM doesn't** — write bare names in C (`isr0`,
  `idt_load`) to meet NASM's `_isr0`, `_idt_load`. Spelling `_isr0` in C
  asks the linker for `__isr0`.
- **QEMU triple-faults / reboots** — run `python build.py headless` and read
  `build/serial.log`; no `HAKU-BOOT-OK` means the kernel never started
  (check disk load / GDT / entry address). A `KERNEL PANIC:` line is followed
  by the crash report and `build/qemu.log` has the exception trace.
- **NASM not found** — install it, then open a fresh terminal (PATH refresh).
- **kernel exceeds 448 KiB** — the stage-2 loader deliberately stops below
  the bootstrap stack. Move to a memory-backed or later-stage loading design
  before increasing this limit.

## Project documentation

- [Roadmap](ROADMAP.md) — current status, milestones, and acceptance criteria.
- [Contributing](CONTRIBUTING.md) — development setup and pull request guidance.
- [Security policy](SECURITY.md) — private vulnerability reporting guidance.
- [Support](SUPPORT.md) — where to ask questions and report reproducible issues.
- [Code of Conduct](CODE_OF_CONDUCT.md) — expectations for project spaces.
- [Agent instructions](AGENTS.md) — repository-wide coding and validation rules.
