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
              └─ kernel/entry.asm (_entry stub) → call _kernel_main
                    └─ kernel/kernel.c: VGA + COM1 output, then hlt loop
```

Memory map:

| Address | Use |
|---|---|
| `0x7C00` | boot sector (512 B) |
| `0x10000–0x12000` | kernel PE image (~2.5 KiB, linked `/BASE:0x10000 /ALIGN:16`) |
| `0x90000` | kernel stack top (grows down) |
| `0xB8000` | VGA text buffer |

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
  kernel/kernel.c      the kernel: VGA driver, COM1 serial, halt loop
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
- **QEMU triple-faults / reboots** — run `python build.py headless` and read
  `build/serial.log`; no `MYOS-BOOT-OK` means the kernel never started
  (check disk load / GDT / entry address).
- **NASM not found** — install it, then open a fresh terminal (PATH refresh).
- **kernel grows past one track (17 sectors)** — extend `boot.asm` with a
  multi-track CHS loop (`mkimg.py` tells you exactly this).

## Next steps (v0.2 ideas)

Interrupt Descriptor Table (IDT) + timer/keyboard IRQs, paging, a tiny
`kmalloc`, user mode (ring 3), and an ATA driver to leave floppies behind.
