; Small freestanding ring-3 payload. The kernel copies this image to
; USER_CODE_BASE, so string references are relative to its image start.
[bits 32]
section .text

global _user_program_start, _user_program_end

%define USER_CODE_BASE 0x40000000

_user_program_start:
    mov eax, 1                   ; SYS_WRITE
    mov ebx, USER_CODE_BASE + (user_message - _user_program_start)
    int 0x80

    mov eax, 0                   ; SYS_EXIT
    int 0x80
.hang:
    jmp .hang

user_message:
    db "USERMODE-OK ring3 syscall=int80", 10, 0
_user_program_end:
