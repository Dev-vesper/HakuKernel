# Haku v0.1 — a tiny x86 operating system in C

Bootloader (NASM) loads a C kernel and switches the CPU to 32-bit protected
mode. The kernel initializes paging, interrupts and a GDT/TSS, then runs a
small user program at ring 3 which writes through `int 0x80` before returning
to the kernel.

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

`python build.py headless` verifies ring-3 process entry, timer preemption,
per-process kernel stacks, syscall handling, process exit/reaping, and timer
IRQs through the serial log.
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
| `0x10000–0x13000` | kernel PE image (~8.5 KiB, linked `/BASE:0x10000 /ALIGN:16`) |
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
memory, the kernel's 32 KiB load buffer, page tables, the boot stack, the ISA
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
`read` (3), `write` (4), and `getpid` (20), with arguments in `ebx`, `ecx`,
and `edx` and syscall number/result in `eax`.

### User-mode system calls

`read` supports stdin from the PS/2 keyboard; `write` supports stdout/stderr
to VGA and serial. Reads and writes are bounded to 4096 bytes, and errors
return negative errno values (a future libc can translate these to `errno`).

Before copying, the kernel validates every covered page in the current page
tables for presence, user access, and write permission where required. It
rejects unmapped/kernel pointers, wraparound, invalid descriptors and
oversized I/O. The syscall entry enables interrupts while in kernel code so
blocking keyboard reads can make progress; the keyboard wait is atomic
against the interrupt/wakeup race. Ring-3 page faults are contained and
reported rather than halting the kernel. These syscalls are an initial
POSIX-inspired kernel interface, not full POSIX conformance. Run
`python build.py headless` to test read/write/getpid/exit, bad pointers,
descriptor errors, input copying, and continued timer operation.

### ELF32 program loading

`kernel/elf.c` loads static ELF32 little-endian i386 `ET_EXEC` images.
It validates the ELF header and program-header bounds, accepts non-overlapping
readable `PT_LOAD` segments, checks address arithmetic and file ranges, and
rejects dynamic/interpreted images and writable-executable segments. Each
segment is mapped according to its ELF permissions; the loader zeroes
`p_memsz - p_filesz` for BSS and creates a two-page user stack.

Each loaded image receives a fresh page directory containing supervisor-only
kernel identity mappings plus only that image's user pages. The scheduler
activates each process's CR3 on dispatch and returns to the kernel page
directory for its idle context. Exited process mappings and page tables are
released after switching away from their kernel stacks. `kernel/user.asm` is
a small ELF32 test executable; `build.py`
assembles it and embeds its bytes into the kernel so the normal headless QEMU
test exercises the actual ELF parser and loader. This initial loader does
not yet support ELF dynamic linking, relocations, shared libraries, or
loading from a filesystem.

### Why `link.exe` + `tools/pe2bin.py`?

MS `link.exe` cannot emit flat binaries and raw COFF objects carry
MSVC-specific relocations, so we let the **real linker** do its job:
`entry.obj + kernel/isr/user objects + kernel C objects` → fixed-base PE
(`/BASE:0x10000 /ENTRY:entry`),
then the Python script only copies section bytes to their RVA — zero
relocation math on our side, zero MSVC quirks to chase. The reported
`ENTRY=` address is baked into `boot.asm` via `nasm -DKERNEL_ENTRY=...`
(`/ALIGN:16` packs sections so the image fits the single-track read).

The A20 routine mirrors Linux `arch/x86/boot/a20.c` (test → BIOS →
test → keyboard controller → long test → fast gate, with the KBC drain
loop), and the COM1 init mirrors Linux `early_serial_init`.

## Project layout

```
Haku/
  boot/boot.asm        16-bit bootloader (disk → Linux-style A20 → GDT → protected mode)
  kernel/entry.asm     32-bit entry stub (_entry, linked first via /ENTRY)
  kernel/isr.asm       interrupt stubs, GDT/TSS load, ring-3 transition + syscalls
  kernel/user.asm      minimal ring-3 demo payload
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
  tools/mkimg.py       size/signature checks + floppy padding
  build.py             the whole build: cl/link/nasm/qemu orchestration
  build/               generated files (boot.bin, kernel.obj/pe, haku.img, ...)
  haku.img             bootable 1.44 MB floppy image
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
- **kernel grows past one track (17 sectors)** — extend `boot.asm` with a
  multi-track CHS loop (`mkimg.py` tells you exactly this).

## Next steps (v0.2 ideas)

Dynamic frame allocation beyond the identity-mapped 8 MiB, process
isolation/address-space switching, and an ATA driver to leave floppies behind.
