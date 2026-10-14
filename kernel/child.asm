; Small child image for spawn/waitpid and exit-status integration coverage.
[bits 32]
[org 0]

%define USER_CODE_BASE 0x40000000
%define USER_DATA_BASE 0x40001000

%include "haku/abi.inc"

    db 0x7F, 'E', 'L', 'F'
    db 1, 1, 1, 0
    times 8 db 0
    dw 2
    dw 3
    dd 1
    dd USER_CODE_BASE
    dd 52
    dd 0
    dd 0
    dw 52
    dw 32
    dw 2
    dw 40, 0, 0

    dd 1, 0x1000, USER_CODE_BASE, USER_CODE_BASE
    dd code_end - code_start, code_end - code_start
    dd 5, 0x1000

    dd 1, 0x2000, USER_DATA_BASE, USER_DATA_BASE
    dd data_end - data_start, data_end - data_start
    dd 6, 0x1000

    times 0x1000 - ($ - $$) db 0
code_start:
    test esp, 15
    jnz .fail
    cmp dword [esp], 0
    jne .fail
    cmp dword [esp + 4], 0
    jne .fail
    cmp dword [esp + 8], 0
    jne .fail
    cmp dword [esp + 12], 0
    jne .fail
    cmp dword [esp + 16], 0
    jne .fail
    mov eax, SYS_WRITE
    mov ebx, 1
    mov ecx, USER_DATA_BASE + (child_message - data_start)
    mov edx, child_message_end - child_message
    int 0x80
    cmp eax, child_message_end - child_message
    jne .fail
    mov eax, SYS_EXIT
    mov ebx, 42
    int 0x80
.hang:
    jmp .hang

.fail:
    mov eax, SYS_EXIT
    mov ebx, 99
    int 0x80
    jmp .hang
code_end:

    times 0x2000 - ($ - $$) db 0
data_start:
child_message:
    db "SPAWN-CHILD-RAN", 10
child_message_end:
data_end:
