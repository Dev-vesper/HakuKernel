# Haku v0.1 — a tiny x86 operating system in C

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
python build.py            :: build haku.img only
python build.py run        :: build + boot in a QEMU window
python build.py headless   :: build + boot headless, verify via serial log
```

`python build.py headless` verifies ATA detection, FAT12 mounting and
multi-cluster file reads, ring-3 process entry, timer preemption, per-process
kernel stacks, syscall handling, process exit/reaping, and timer IRQs through
the serial log.
The build is one Python script on purpose — `cmd.exe` quoting of spaced
paths (e.g. `C:\Program Files\...`) is fragile, while `subprocess` argument
lists just work.

## How it works

```
BIOS (0x7C00)
  └─ boot/stage1.asm + boot/stage2.asm (16-bit): load kernel at 0x10000
     → A20 (Linux-style: test after BIOS/KBC/fast methods) → GDT → CR0.PE=1
        └─ far jump to KERNEL_ENTRY (= PE entry point from link.exe)
              └─ kernel/entry.asm (_entry stub) → call kernel_main
                    └─ kernel/kernel.c: paging/IDT → GDT+TSS → scheduler
                          └─ timer-preempted ring-3 processes → int 0x80
```

Memory map:

| Address | Use |
|---|---|
| `0x7C00` | boot sector (512 B) |
| `0x10000–0x80000` | kernel load window (up to 448 KiB, linked `/BASE:0x10000 /ALIGN:16`) |
| `0x7000–0x7240` | E820 map from stage2 (INT 15h, count at `0x6FFC`) |
| `0x20000–0x23000` | page directory + initial tables (8 MiB supervisor identity map) |
| `0x100000–0x400000` | kernel heap (kmalloc/kfree, 16 B aligned) |
| `0x90000` | bootstrap kernel stack top (grows down) |
| below `0x800000` | allocated per-process 8 KiB kernel stacks |
| `0x40000000` | user program code/data virtual base (per-process mapping) |
| `0xBFFFE000–0xC0000000` | two user stack pages |
| `0xB8000` | VGA text buffer |

### Interrupts (IDT)

`kernel/isr.asm` builds a uniform stack frame for all 256 vectors (dummy
error code where the CPU pushes none), saves all data segments, and calls
into C. `kernel/idt.c`
installs 32 exception gates (0–31), 16 IRQ gates (32–47, used once the
8259 is remapped) and a catch-all for 48–255 that reports instead of
triple-faulting. The syscall gate at vector 128 is DPL 3; all other gates
remain ring-0-only. All are interrupt gates, so handlers run with IF clear.
The boot proves the exception path with an `INT3` self-test.

### Physical frame allocator

`kernel/frame.c` now builds its usable-frame bitmap from the 64-bit base and
length fields in the E820 entries. Only complete 4 KiB pages wholly inside
type-1 usable ranges are admitted; invalid entries and overlapping reserved
ranges are excluded. The allocator explicitly reserves low boot/firmware
memory, the kernel's 448 KiB load window, page tables, the boot stack, the ISA
hole, and the 1–4 MiB heap. `frame_usable_count()` reports E820-usable pages
tracked up to 128 MiB, while allocations are currently restricted to the
identity-mapped first 8 MiB. Freeing validates alignment, usability, and
allocation state so reserved pages cannot accidentally be released.

### Processes and preemptive scheduling

`kernel/scheduler.c` runs two instances of the ELF test program with separate
address spaces and kernel stacks. The 100 Hz PIT preempts runnable processes
by saving the complete interrupt frame and restoring another process's frame
through `iret`. Each process has an 8 KiB ring-0 stack; `gdt_set_kernel_stack`
updates `TSS.esp0` at every switch so the next ring-3 interrupt or syscall
enters on the correct stack. User exceptions and `exit` mark a process for
reaping; its address space and kernel stack are reclaimed after execution has
moved to another stack. When all processes exit, the scheduler runs a
dedicated kernel idle context.

`kernel/gdt.c` installs flat ring-0/ring-3 code and data descriptors plus a
32-bit TSS. `kernel/paging.c` gives each process supervisor-only kernel
identity mappings and maps its user program and two-page user stack with
user permissions.

`kernel/user.asm` is built as an ELF32 image, loaded separately for each
process and entered via `iret` at CPL 3. The initial syscall ABI follows the
i386 Linux syscall numbers for a small POSIX-oriented subset: `exit` (1),
`read` (3), `write` (4), `open` (5), `close` (6), `lseek` (19), and `getpid`
(20), with arguments in `ebx`, `ecx`, and `edx` and syscall number/result in
`eax`.

### User-mode system calls

`read` supports stdin from the PS/2 keyboard and regular files; `write`
supports stdout/stderr to VGA and serial as well as writable regular files.
`open`, `close`, `lseek`, and per-process file descriptors are backed by the
VFS/FAT12 layer. Reads and writes are bounded to 4096 bytes, and errors return
negative errno values (a future libc can translate these to `errno`).
Every FAT12 failure class keeps one distinct errno end to end: missing
files give `ENOENT`, bad flags/whence/pointers give `EINVAL`, existing
files give `EEXIST`, bad descriptors give `EBADF`, a full volume gives
`ENOSPC`, and disk I/O failures give `EIO`. After both user processes
exit, the idle task runs an error self-test (`FS-ERRORS-OK`) including a
root-fill probe that must fail with `ENOSPC` (`FS-ENOSPC-OK`). The same
pass also covers empty files, exact-sector and boundary-spanning I/O
(`FS-FILE-EDGE-OK`), injected cut/bad/reserved/cyclic FAT chains (each
restored, reads must give `EIO`, loops must terminate) plus a crafted
full data area (`FS-CORRUPT-OK`), and ATA bounds/NULL-transfer rejection
(`FS-ATA-OK`).

Before copying, the kernel validates every covered page in the current page
tables for presence, user access, and write permission where required. It
rejects unmapped/kernel pointers, wraparound, invalid descriptors and
oversized I/O. The syscall entry enables interrupts while in kernel code so
blocking keyboard reads can make progress; the keyboard wait is atomic
against the interrupt/wakeup race. Ring-3 page faults are contained and
reported rather than halting the kernel. These syscalls are an initial
POSIX-inspired kernel interface, not full POSIX conformance. Run
`python build.py headless` to test read/write/open/close/lseek/getpid/exit,
bad pointers, descriptor errors, input copying, disk-backed create/write/read
across a sector boundary, and continued timer operation.

### ATA PIO and FAT12

`kernel/ata.c` detects the primary-master ATA device with IDENTIFY and reads
and writes LBA28 sectors using bounded polling PIO. `kernel/fat12.c` mounts a
FAT12 superfloppy and supports creating, reading, writing, truncating and
seeking 8.3-named files in its root directory, with cluster allocation,
release, mirrored FAT updates and volume/chain bounds checks.
`kernel/vfs.c` supplies per-process descriptors and the user `open`, `close`,
`read`, `write`, and `lseek` interface. Disk I/O is serialized against the
preemptive scheduler; file contents and directory/FAT metadata are written
through to the ATA disk. The current VFS supports up to five simultaneously
open regular files per process and the `O_RDONLY`, `O_WRONLY`, `O_RDWR`,
`O_CREAT`, `O_EXCL`, `O_TRUNC`, and `O_APPEND` flags.

The filesystem remains intentionally small: no partition tables, subfolders,
long filenames, unlink/rename, permissions, mount points, journaling, FAT16,
or FAT32. This is a minimal single-volume VFS, not full POSIX filesystem
conformance. The QEMU headless test creates separate `P1.TXT` and `P2.TXT`
files from the two user processes and verifies multi-sector writes, reads,
seeks, close, and reopen.

`tools/mkfat12.py` puts the assembled `HAKU.ELF` test program on a generated
1.44 MB FAT12 image. QEMU attaches this image as the primary-master IDE disk
while the existing boot floppy still starts the kernel. At startup, the
kernel reads the ELF file into temporary heap memory, loads it into separate
user address spaces and releases the temporary buffer.

### ELF32 program loading

`kernel/elf.c` loads static ELF32 little-endian i386 `ET_EXEC` images read
from FAT12. It validates the ELF header and program-header bounds, accepts
non-overlapping readable `PT_LOAD` segments, checks address arithmetic and
file ranges, and rejects dynamic/interpreted images and writable-executable
segments. Each segment is mapped according to its ELF permissions; the loader
zeroes `p_memsz - p_filesz` for BSS and creates a two-page user stack.

Each loaded image receives a fresh page directory containing supervisor-only
kernel identity mappings plus only that image's user pages. The scheduler
activates each process's CR3 on dispatch and returns to the kernel page
directory for its idle context. Exited process mappings and page tables are
released after switching away from their kernel stacks. Dynamic linking,
relocations, shared libraries, and ELF files larger than the current 256 KiB
loader buffer are not supported.

### Why `link.exe` + `tools/pe2bin.py`?

MS `link.exe` cannot emit flat binaries and raw COFF objects carry
MSVC-specific relocations, so we let the **real linker** do its job:
`entry.obj + kernel/isr/user objects + kernel C objects` → fixed-base PE
(`/BASE:0x10000 /ENTRY:entry`),
then the Python script only copies section bytes to their RVA — zero
relocation math on our side, zero MSVC quirks to chase. The reported
`ENTRY=` address and exact padded kernel sector count are passed to stage 2.
The stage-2 CHS loop reads track-bounded chunks into the 448 KiB load window,
leaving 64 KiB below it for the bootstrap stack; `mkimg.py` enforces both
the memory and floppy-image limits.

The A20 routine mirrors Linux `arch/x86/boot/a20.c` (test → BIOS →
test → keyboard controller → long test → fast gate, with the KBC drain
loop), and the COM1 init mirrors Linux `early_serial_init`.

## Project layout

```
Haku/
  boot/stage1.asm      16-bit boot sector; loads stage 2
  boot/stage2.asm      CHS kernel loader (up to 448 KiB) → A20 → GDT → protected mode
  kernel/entry.asm     32-bit entry stub (_entry, linked first via /ENTRY)
  kernel/isr.asm       interrupt stubs, GDT/TSS load, ring-3 transition + syscalls
  kernel/user.asm      minimal ELF32 ring-3 test program
  kernel/ata.h/.c      primary-master ATA LBA28 polling PIO driver
  kernel/fat12.h/.c    writable FAT12 root-directory file operations
  kernel/vfs.h/.c      per-process descriptors and file syscalls
  kernel/elf.h/.c      validated ELF32 PT_LOAD loader and fresh user address spaces
  kernel/cpu.h/cpu.c   types, port I/O, tiny libc (memset & friends, no CRT)
  kernel/console.h/.c  VGA text + COM1 serial drivers
  kernel/idt.h/idt.c   IDT gates, exception/IRQ dispatchers (+ #PF handler)
  kernel/gdt.h/.c      flat ring-0/ring-3 GDT and dedicated TSS stack
  kernel/usermode.h/.c user mappings, bounded write/exit syscall ABI
  kernel/paging.h/.c   i386 page tables: 8 MiB identity map + user pages
  kernel/frame.h/.c    E820-backed frame inventory (128 MiB tracked, allocations within mapped 8 MiB)
  kernel/kernel.c      kernel startup, ring-3 demo, PIC/PIT + keyboard loop
  tools/pe2bin.py      PE image → flat binary (+ reports ENTRY= for the bootloader)
  tools/mkimg.py       boot floppy size/signature checks
  tools/mkfat12.py     FAT12 image builder for the user ELF
  build.py             the whole build: cl/link/nasm/qemu orchestration
  build/               generated files (stage*.bin, kernel.obj/pe, haku.img, ...)
  haku.img             bootable floppy image
  build/haku_fs.img    1.44 MB FAT12 ATA disk image with HAKU.ELF
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
  (check disk load / GDT / entry address).
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
