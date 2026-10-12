# MyOS v0.1 — a tiny x86 operating system in C

Bootloader (NASM) loads a C kernel, switches the CPU to 32-bit protected
mode, and the kernel prints to VGA text mode + serial port, then halts.

## Prerequisites (Windows 11)

| Tool | Version here | Notes |
|---|---|---|
| Visual Studio 2022 | 17.x, **Desktop development with C++** | we use the x86 `cl.exe` directly — no project/solution needed |
| NASM | 3.02 | `winget install -e --id NASM.NASM` |
| QEMU | 11.x | `C:\Program Files\qemu\qemu-system-i386.exe` |
| Python 3 | 3.14 | drives the whole build (`build.py` + helpers) |

## Build & run

```cmd
cd D:\Source\MyOS
python build.py            :: build myos.img only
python build.py run        :: build + boot in a QEMU window
python build.py headless   :: build + boot headless, verify via serial log
```

Expected on screen: blue background, `MyOS v0.1 - hello from the C kernel`.
The build is one Python script on purpose — `cmd.exe` quoting of spaced
paths (e.g. `C:\Program Files\...`) is fragile, while `subprocess` argument
lists just work.

## How it works

```
BIOS (0x7C00)
  └─ boot/boot.asm (16-bit): save DL → single-track read to 0x10000
     → A20 (Linux-style: test after BIOS/KBC/fast methods) → GDT → CR0.PE=1
        └─ far jump to KERNEL_ENTRY (= PE entry point from link.exe)
              └─ kernel/entry.asm (_entry stub) → call kernel_main
                    └─ kernel/kernel.c: console → idt_init → INT3 self-test → hlt
```

Memory map:

| Address | Use |
|---|---|
| `0x7C00` | boot sector (512 B) |
| `0x10000–0x13000` | kernel PE image (~8.5 KiB, linked `/BASE:0x10000 /ALIGN:16`) |
| `0x6000–0x7000` | E820 map from stage2 (INT 15h, count at `0x6FFC`) |
| `0x20000–0x23000` | page directory + 2 page tables (8 MiB identity map, P+W) |
| `0x100000–0x400000` | kernel heap (kmalloc/kfree, 16 B aligned) |
| `0x90000` | kernel stack top (grows down) |
| `0xB8000` | VGA text buffer |

### Interrupts (IDT)

`kernel/isr.asm` builds a uniform stack frame for all 256 vectors (dummy
error code where the CPU pushes none) and calls into C. `kernel/idt.c`
installs 32 exception gates (0–31), 16 IRQ gates (32–47, used once the
8259 is remapped) and a catch-all for 48–255 that reports instead of
triple-faulting. All gates are interrupt gates (`0x8E`), so handlers never
nest. The boot proves the whole path with an `INT3` self-test: the serial
log must show `EXC-03-BP` followed by `INT3-RESUMED`.

Next: E820-driven freeing past 8 MiB (needs dynamic page mapping), then user mode (ring 3).

### Why `link.exe` + `tools/pe2bin.py`?

MS `link.exe` cannot emit flat binaries and raw COFF objects carry
MSVC-specific relocations, so we let the **real linker** do its job:
`entry.obj + kernel.obj` → fixed-base PE (`/BASE:0x10000 /ENTRY:entry`),
then the Python script only copies section bytes to their RVA — zero
relocation math on our side, zero MSVC quirks to chase. The reported
`ENTRY=` address is baked into `boot.asm` via `nasm -DKERNEL_ENTRY=...`
(`/ALIGN:16` packs sections so the image fits the single-track read).

The A20 routine mirrors Linux `arch/x86/boot/a20.c` (test → BIOS →
test → keyboard controller → long test → fast gate, with the KBC drain
loop), and the COM1 init mirrors Linux `early_serial_init`.

## Project layout

```
MyOS/
  boot/boot.asm        16-bit bootloader (disk → Linux-style A20 → GDT → protected mode)
  kernel/entry.asm     32-bit entry stub (_entry, linked first via /ENTRY)
  kernel/isr.asm       interrupt stubs: 32 exceptions + 16 IRQs + catch-all
  kernel/cpu.h/cpu.c   types, port I/O, tiny libc (memset & friends, no CRT)
  kernel/console.h/.c  VGA text + COM1 serial drivers
  kernel/idt.h/idt.c   IDT gates, exception/IRQ dispatchers (+ #PF handler)
  kernel/paging.h/.c   i386 2-level tables (Linux-style): 8 MiB identity map, CR0.PG
  kernel/frame.h/.c    bitmap frame allocator (4 KiB frames, E820 handoff at 0x6FFC/0x7000)
  kernel/kernel.c      kernel_main: console → IDT → INT3 self-test → paging → PIC/PIT → halt
  tools/pe2bin.py      PE image → flat binary (+ reports ENTRY= for the bootloader)
  tools/mkimg.py       size/signature checks + floppy padding
  build.py             the whole build: cl/link/nasm/qemu orchestration
  build/               generated files (boot.bin, kernel.obj/pe, myos.img, ...)
  myos.img             bootable 1.44 MB floppy image
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
  `build/serial.log`; no `MYOS-BOOT-OK` means the kernel never started
  (check disk load / GDT / entry address).
- **NASM not found** — install it, then open a fresh terminal (PATH refresh).
- **kernel grows past one track (17 sectors)** — extend `boot.asm` with a
  multi-track CHS loop (`mkimg.py` tells you exactly this).

## Next steps (v0.2 ideas)

Interrupt Descriptor Table (IDT) + timer/keyboard IRQs, paging, a tiny
`kmalloc`, user mode (ring 3), and an ATA driver to leave floppies behind.
