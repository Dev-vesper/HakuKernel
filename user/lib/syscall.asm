[bits 32]

%include "haku/abi.inc"

section .text

extern _haku_errno_check

; vfork(2): the child runs on the parent's stack, so the pending return
; address must not sit there while the syscall is in flight - the child's
; own calls would overwrite it. edx survives the syscall, so it waits there.
global _vfork
_vfork:
    pop edx
    mov eax, HAKU_SYS_VFORK
    int 0x80
    push eax
    call _haku_errno_check
    add esp, 4
    push edx
    ret

global _haku_syscall0
_haku_syscall0:
    push ebx
    push esi
    push edi
    mov eax, [esp + 16]
    int 0x80
    pop edi
    pop esi
    pop ebx
    ret

global _haku_syscall1
_haku_syscall1:
    push ebx
    push esi
    push edi
    mov eax, [esp + 16]
    mov ebx, [esp + 20]
    int 0x80
    pop edi
    pop esi
    pop ebx
    ret

global _haku_syscall2
_haku_syscall2:
    push ebx
    push esi
    push edi
    mov eax, [esp + 16]
    mov ebx, [esp + 20]
    mov ecx, [esp + 24]
    int 0x80
    pop edi
    pop esi
    pop ebx
    ret

global _haku_syscall3
_haku_syscall3:
    push ebx
    push esi
    push edi
    mov eax, [esp + 16]
    mov ebx, [esp + 20]
    mov ecx, [esp + 24]
    mov edx, [esp + 28]
    int 0x80
    pop edi
    pop esi
    pop ebx
    ret

global _haku_syscall4
_haku_syscall4:
    push ebx
    push esi
    push edi
    mov eax, [esp + 16]
    mov ebx, [esp + 20]
    mov ecx, [esp + 24]
    mov edx, [esp + 28]
    mov esi, [esp + 32]
    int 0x80
    pop edi
    pop esi
    pop ebx
    ret
