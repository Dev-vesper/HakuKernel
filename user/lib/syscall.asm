[bits 32]
section .text

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
