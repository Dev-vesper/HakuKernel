[bits 32]
section .text

%include "haku/abi.inc"

global _start
extern _main

_start:
    call _main
    mov ebx, eax
    mov eax, HAKU_SYS_EXIT
    int 0x80
    hlt
