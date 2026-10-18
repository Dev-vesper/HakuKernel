; Second ELF image used to verify execve replacement and descriptor survival.
[bits 32]
[org 0]

%define USER_CODE_BASE 0x40000000
%define USER_DATA_BASE 0x40001000

%include "haku/abi.inc"
%define EXPECTED_ARGC 3

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
    dd data_file_end - data_start, data_end - data_start
    dd 6, 0x1000

    times 0x1000 - ($ - $$) db 0
code_start:
    ; The initial stack follows argc, argv[], NULL, envp[], NULL, AT_NULL.
    test esp, 15
    jnz .fail
    cmp dword [esp], EXPECTED_ARGC
    jne .fail
    mov esi, [esp + 4]
    mov edi, USER_DATA_BASE + (expected_arg0 - data_start)
    mov ecx, expected_arg0_end - expected_arg0
    cld
    repe cmpsb
    jne .fail
    mov esi, [esp + 8]
    mov edi, USER_DATA_BASE + (expected_arg1 - data_start)
    mov ecx, expected_arg1_end - expected_arg1
    repe cmpsb
    jne .fail
    mov esi, [esp + 12]
    mov edi, USER_DATA_BASE + (expected_arg2 - data_start)
    mov ecx, expected_arg2_end - expected_arg2
    repe cmpsb
    jne .fail
    cmp dword [esp + 16], 0
    jne .fail
    mov esi, [esp + 20]
    mov edi, USER_DATA_BASE + (expected_env0 - data_start)
    mov ecx, expected_env0_end - expected_env0
    repe cmpsb
    jne .fail
    mov esi, [esp + 24]
    mov edi, USER_DATA_BASE + (expected_env1 - data_start)
    mov ecx, expected_env1_end - expected_env1
    repe cmpsb
    jne .fail
    cmp dword [esp + 28], 0
    jne .fail
    cmp dword [esp + 32], 0
    jne .fail
    cmp dword [esp + 36], 0
    jne .fail

    ; exec keeps the process identity.
    mov eax, SYS_GETPID
    int 0x80
    cmp eax, 1
    jne .fail

    ; fd 3 was open on HAKU.ELF before exec and must still be usable.
    mov eax, SYS_READ
    mov ebx, 3
    mov ecx, USER_DATA_BASE + (saved_file_header - data_start)
    mov edx, 4
    int 0x80
    cmp eax, 4
    jne .fail
    cmp dword [USER_DATA_BASE + (saved_file_header - data_start)], 0x464C457F
    jne .fail
    mov eax, SYS_CLOSE
    mov ebx, 3
    int 0x80
    test eax, eax
    jne .fail

    mov eax, SYS_WRITE
    mov ebx, 1
    mov ecx, USER_DATA_BASE + (success_message - data_start)
    mov edx, success_message_end - success_message
    int 0x80
    cmp eax, edx
    jne .fail
    mov eax, SYS_EXIT
    xor ebx, ebx
    int 0x80
.hang:
    jmp .hang

.fail:
    mov eax, SYS_WRITE
    mov ebx, 2
    mov ecx, USER_DATA_BASE + (failure_message - data_start)
    mov edx, failure_message_end - failure_message
    int 0x80
    mov eax, SYS_EXIT
    mov ebx, 99
    int 0x80
    jmp .hang
code_end:

    times 0x2000 - ($ - $$) db 0
data_start:
success_message:
    db "USER-EXEC-TARGET-OK image, argv/envp, PID and fd preserved", 10
success_message_end:
failure_message:
    db "USER-EXEC-TARGET-FAIL", 10
failure_message_end:
saved_file_header:
    times 4 db 0
expected_arg0:
    db "NEXT.ELF", 0
expected_arg0_end:
expected_arg1:
    db "arg one", 0
expected_arg1_end:
expected_arg2:
    db "arg two", 0
expected_arg2_end:
expected_env0:
    db "MODE=exec-test", 0
expected_env0_end:
expected_env1:
    db "EMPTY=", 0
expected_env1_end:
data_file_end:
data_end:
