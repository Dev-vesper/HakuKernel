; Ring-3 syscall self-test using the i386 POSIX read/write/exit/getpid ABI.
[bits 32]
section .text

global _user_program_start, _user_program_end

%define USER_CODE_BASE 0x40000000
%define USER_STACK_TOP 0xC0000000

%define SYS_EXIT   1
%define SYS_READ   3
%define SYS_WRITE  4
%define SYS_GETPID 20

_user_program_start:
    ; write(1, startup, startup_len)
    mov eax, SYS_WRITE
    mov ebx, 1
    mov ecx, USER_CODE_BASE + (startup_message - _user_program_start)
    mov edx, startup_message_end - startup_message
    int 0x80
    cmp eax, edx
    jne .fail

    ; getpid() -> 1 for the initial process.
    mov eax, SYS_GETPID
    int 0x80
    cmp eax, 1
    jne .fail

    ; write(1, 0x1000, 4) must return -EFAULT without a kernel fault.
    mov eax, SYS_WRITE
    mov ebx, 1
    mov ecx, 0x1000
    mov edx, 4
    int 0x80
    cmp eax, -14
    jne .fail

    ; write(99, startup, 1) must return -EBADF.
    mov eax, SYS_WRITE
    mov ebx, 99
    mov ecx, USER_CODE_BASE + (startup_message - _user_program_start)
    mov edx, 1
    int 0x80
    cmp eax, -9
    jne .fail

    ; A buffer crossing into an unmapped page must be rejected as a whole.
    mov eax, SYS_WRITE
    mov ebx, 1
    mov ecx, USER_CODE_BASE + 0xFFF
    mov edx, 2
    int 0x80
    cmp eax, -14
    jne .fail

    ; Reject arithmetic wraparound and invalid output buffers before blocking.
    mov eax, SYS_READ
    mov ebx, 0
    mov ecx, 0xFFFFFFFC
    mov edx, 8
    int 0x80
    cmp eax, -14
    jne .fail
    mov eax, SYS_READ
    mov ebx, 0
    mov ecx, 0x1000
    mov edx, 1
    int 0x80
    cmp eax, -14
    jne .fail

    ; Unknown syscall numbers return -ENOSYS; oversized I/O returns -EINVAL.
    mov eax, 999
    int 0x80
    cmp eax, -38
    jne .fail
    mov eax, SYS_WRITE
    mov ebx, 1
    mov ecx, USER_CODE_BASE + (startup_message - _user_program_start)
    mov edx, 4097
    int 0x80
    cmp eax, -22
    jne .fail

    mov eax, SYS_WRITE
    mov ebx, 1
    mov ecx, USER_CODE_BASE + (checks_message - _user_program_start)
    mov edx, checks_message_end - checks_message
    int 0x80
    cmp eax, edx
    jne .fail

    ; read(0, NULL, 0) is valid and must not dereference NULL.
    mov eax, SYS_READ
    mov ebx, 0
    xor ecx, ecx
    xor edx, edx
    int 0x80
    test eax, eax
    jne .fail

    ; read one character into the user stack (QEMU headless sends "a").
    mov eax, SYS_READ
    mov ebx, 0
    mov ecx, USER_STACK_TOP - 16
    mov edx, 1
    int 0x80
    cmp eax, 1
    jne .fail
    cmp byte [USER_STACK_TOP - 16], 'a'
    jne .fail

    ; Confirm range-checked copy_to_user and input delivery.
    mov eax, SYS_WRITE
    mov ebx, 1
    mov ecx, USER_CODE_BASE + (read_message - _user_program_start)
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
    mov ecx, USER_CODE_BASE + (failure_message - _user_program_start)
    mov edx, failure_message_end - failure_message
    int 0x80
    mov eax, SYS_EXIT
    mov ebx, 1
    int 0x80
    jmp .hang

startup_message:
    db "USERMODE-OK ring3 POSIX read/write/getpid", 10
startup_message_end:
read_message:
    db "USER-READ-OK copy_to_user and blocking input", 10
read_message_end:
checks_message:
    db "USER-SYSCALL-CHECKS-OK", 10
checks_message_end:
failure_message:
    db "USER-SYSCALL-FAIL", 10
failure_message_end:
_user_program_end:
