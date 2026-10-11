; =============================================================================
; MyOS v0.1 — 32-bit kernel entry stub (NASM -> COFF object for link.exe)
;
; Assembled with `nasm -f win32` and linked FIRST on the link.exe command
; line, so the image starts here. link.exe /ENTRY:_entry jumps to this code
; with flat segments; the C stack convention needs nothing else set up here
; (the bootloader already loaded ESP). kernel_main never returns; if it did
; we fall into hang.
; =============================================================================

[bits 32]

global _entry
extern _kernel_main

section .text

_entry:
    call _kernel_main
.hang:
    hlt
    jmp .hang
