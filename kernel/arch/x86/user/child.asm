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
    ; The initial stack: argc, argv[], NULL, envp[], NULL, then the
    ; auxiliary vector, which ends with AT_NULL.
    test esp, 15
    jnz .fail
    cmp dword [esp], 0
    jne .fail
    cmp dword [esp + 4], 0
    jne .fail
    cmp dword [esp + 8], 0
    jne .fail
    lea esi, [esp + 12]
    mov ecx, 16
    xor ebx, ebx                 ; page size, if the vector carries it
.auxv:
    mov eax, [esi]
    test eax, eax
    jz .auxv_end
    cmp eax, HAKU_AT_PAGESZ
    jne .auxv_next
    mov ebx, [esi + 4]
.auxv_next:
    add esi, 8
    dec ecx
    jnz .auxv
    jmp .fail                    ; the vector has no AT_NULL
.auxv_end:
    cmp ebx, 4096
    jne .fail

    ; fd 3 was inherited from the parent and holds known bytes: the child
    ; sees the same open file description the parent does.
    mov eax, SYS_READ
    mov ebx, 3
    mov ecx, USER_DATA_BASE + (inherit_check - data_start)
    mov edx, expected_payload_end - expected_payload
    int 0x80
    cmp eax, expected_payload_end - expected_payload
    jne .fail
    mov esi, USER_DATA_BASE + (expected_payload - data_start)
    mov edi, USER_DATA_BASE + (inherit_check - data_start)
    mov ecx, expected_payload_end - expected_payload
    cld
    repe cmpsb
    jne .fail

    ; Close it: only the child's reference goes away, the parent keeps its
    ; own copy of the shared description.
    mov eax, SYS_CLOSE
    mov ebx, 3
    int 0x80
    test eax, eax
    jne .fail

    mov eax, SYS_WRITE
    mov ebx, 1
    mov ecx, USER_DATA_BASE + (child_message - data_start)
    mov edx, child_message_end - child_message
    int 0x80
    cmp eax, child_message_end - child_message
    jne .fail

    ; Exit with a descriptor still open on an unlinked file; the automatic
    ; close-on-exit must release it (and free the removed data).
    mov eax, SYS_OPEN
    mov ebx, USER_DATA_BASE + (scratch_path - data_start)
    mov ecx, 0x242             ; O_CREAT | O_RDWR | O_TRUNC
    xor edx, edx
    int 0x80
    cmp eax, 3
    jb .fail
    mov edi, eax
    mov eax, SYS_WRITE
    mov ebx, edi
    mov ecx, USER_DATA_BASE + (child_message - data_start)
    mov edx, 1
    int 0x80
    cmp eax, 1
    jne .fail
    mov eax, SYS_UNLINK
    mov ebx, USER_DATA_BASE + (scratch_path - data_start)
    int 0x80
    test eax, eax
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
expected_payload:
    db "INHERITED-FD"
expected_payload_end:
scratch_path:
    db "KIDW.TMP", 0
inherit_check:
    times expected_payload_end - expected_payload db 0
data_end:
