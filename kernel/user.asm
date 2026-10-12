; Minimal static ELF32/i386 executable used to exercise the ELF loader.
[bits 32]
[org 0]

%define USER_CODE_BASE 0x40000000
%define USER_DATA_BASE 0x40001000
%define USER_STACK_TOP 0xC0000000

%define SYS_EXIT   1
%define SYS_READ   3
%define SYS_WRITE  4
%define SYS_GETPID 20

; ELF32 header
    db 0x7F, 'E', 'L', 'F'
    db 1, 1, 1, 0
    times 8 db 0
    dw 2                          ; ET_EXEC
    dw 3                          ; EM_386
    dd 1                          ; EV_CURRENT
    dd USER_CODE_BASE             ; entry point
    dd 52                         ; program-header table offset
    dd 0                          ; no section table
    dd 0                          ; flags
    dw 52                         ; ELF header size
    dw 32                         ; program-header size
    dw 2                          ; two PT_LOAD segments
    dw 40, 0, 0                   ; no section headers

; RX code segment: file offset 0x1000, virtual address 0x40000000.
    dd 1, 0x1000, USER_CODE_BASE, USER_CODE_BASE
    dd code_end - code_start, code_end - code_start
    dd 5, 0x1000

; RW data segment: file offset 0x2000, virtual address 0x40001000.
    dd 1, 0x2000, USER_DATA_BASE, USER_DATA_BASE
    dd data_file_end - data_start, data_end - data_start
    dd 6, 0x1000

    times 0x1000 - ($ - $$) db 0
code_start:
_user_program_start:
    ; write(1, startup, startup_len)
    mov esi, 1
    mov eax, SYS_WRITE
    mov ebx, 1
    mov ecx, USER_DATA_BASE + (startup_message - data_start)
    mov edx, startup_message_end - startup_message
    int 0x80
    cmp eax, edx
    jne .fail

    ; Both scheduled processes must receive distinct valid PIDs.
    mov esi, 2
    mov eax, SYS_GETPID
    int 0x80
    cmp eax, 1
    jb .fail
    cmp eax, 2
    ja .fail

    ; The data segment's BSS tail must be zero-filled by the ELF loader.
    mov esi, 2
    cmp dword [USER_DATA_BASE + (bss_word - data_start)], 0
    jne .fail
    mov dword [USER_DATA_BASE + (bss_word - data_start)], 0x12345678
    cmp dword [USER_DATA_BASE + (bss_word - data_start)], 0x12345678
    jne .fail

    ; Unmapped, kernel, cross-page, and wrapping ranges return -EFAULT.
    mov esi, 3
    mov eax, SYS_WRITE
    mov ebx, 1
    mov ecx, 0x1000
    mov edx, 4
    int 0x80
    cmp eax, -14
    jne .fail
    mov esi, 4
    mov eax, SYS_WRITE
    mov ebx, 1
    mov ecx, 0x100000
    mov edx, 1
    int 0x80
    cmp eax, -14
    jne .fail
    mov esi, 5
    mov eax, SYS_WRITE
    mov ebx, 1
    mov ecx, USER_DATA_BASE + 0xFFF
    mov edx, 2
    int 0x80
    cmp eax, -14
    jne .fail
    mov esi, 6
    mov eax, SYS_READ
    mov ebx, 0
    mov ecx, 0xFFFFFFFC
    mov edx, 8
    int 0x80
    cmp eax, -14
    jne .fail
    mov esi, 7
    mov eax, SYS_READ
    mov ebx, 0
    mov ecx, 0x1000
    mov edx, 1
    int 0x80
    cmp eax, -14
    jne .fail
    ; An RX code page is readable but must not be a read() destination.
    mov eax, SYS_READ
    xor ebx, ebx
    mov ecx, USER_CODE_BASE
    mov edx, 1
    int 0x80
    cmp eax, -14
    jne .fail

    ; Invalid fd, unknown syscall, and oversized I/O return errno values.
    mov esi, 8
    mov eax, SYS_WRITE
    mov ebx, 99
    mov ecx, USER_DATA_BASE + (startup_message - data_start)
    mov edx, 1
    int 0x80
    cmp eax, -9
    jne .fail
    mov esi, 9
    mov eax, 999
    int 0x80
    cmp eax, -38
    jne .fail
    mov esi, 10
    mov eax, SYS_WRITE
    mov ebx, 1
    mov ecx, USER_DATA_BASE + (startup_message - data_start)
    mov edx, 4097
    int 0x80
    cmp eax, -22
    jne .fail

    mov esi, 11
    mov eax, SYS_WRITE
    mov ebx, 1
    mov ecx, USER_DATA_BASE + (checks_message - data_start)
    mov edx, checks_message_end - checks_message
    int 0x80
    cmp eax, edx
    jne .fail

    ; read(0, NULL, 0) is valid; read one arbitrary key into user stack.
    mov esi, 12
    mov eax, SYS_READ
    xor ebx, ebx
    xor ecx, ecx
    xor edx, edx
    int 0x80
    test eax, eax
    jne .fail
    mov esi, 13
    mov eax, SYS_READ
    xor ebx, ebx
    mov ecx, USER_STACK_TOP - 16
    mov edx, 1
    int 0x80
    cmp eax, 1
    jne .fail

    mov esi, 14
    mov eax, SYS_WRITE
    mov ebx, 1
    mov ecx, USER_DATA_BASE + (read_message - data_start)
    mov edx, read_message_end - read_message
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
    mov ebx, esi
    int 0x80
    jmp .hang
code_end:

    times 0x2000 - ($ - $$) db 0
data_start:
startup_message:
    db "USERMODE-OK ELF32 RX/RW segments + read/write/getpid", 10
startup_message_end:
read_message:
    db "USER-READ-OK ELF data and stack copy_to_user", 10
read_message_end:
checks_message:
    db "USER-SYSCALL-CHECKS-OK", 10
checks_message_end:
failure_message:
    db "USER-SYSCALL-FAIL", 10
failure_message_end:
data_file_end:
    times 32 db 0
bss_word:
    dd 0
data_end:
