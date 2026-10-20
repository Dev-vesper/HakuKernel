; Protection-fault child: performs one illegal access selected by a byte
; read from inherited fd 3 and relies on the kernel to terminate it. The
; parent checks the encoded #PF error code in the waitpid status.
;
;   'C'  write to the read-only code segment      -> #PF err 0x7
;   'R'  write to a PROT_READ anonymous mapping   -> #PF err 0x7
;   'U'  read from an unmapped user address       -> #PF err 0x4
;   'F'  write through a munmap'd mapping (UAF)   -> #PF err 0x6
;
; Any access that does not fault exits 99, which the parent rejects.
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
    mov eax, SYS_READ
    mov ebx, 3
    mov ecx, USER_DATA_BASE + (mode_byte - data_start)
    mov edx, 1
    int 0x80
    cmp eax, 1
    jne fail
    mov al, [USER_DATA_BASE + (mode_byte - data_start)]
    cmp al, 'C'
    je .code_write
    cmp al, 'R'
    je .read_only
    cmp al, 'U'
    je .unmapped
    cmp al, 'F'
    je .use_after_free
    jmp fail

    ; The image's PT_LOAD with PF_X has no PAGE_RW, so a store must fault.
.code_write:
    mov dword [USER_CODE_BASE + 0x40], 0x11111111
    jmp fail

    ; PROT_READ mmap: readable, but a store must fault.
.read_only:
    mov dword [USER_DATA_BASE + (mmap_arg - data_start) + 8], HAKU_PROT_READ
    call mmap_one_page
    cmp dword [eax], 0
    jne fail
    mov dword [eax], 0x22222222
    jmp fail

    ; Nothing is mapped at 0x10000000 in a user address space.
.unmapped:
    mov edx, [0x10000000]
    jmp fail

    ; Map, touch, release, then store through the stale address: the PTE
    ; was cleared and the frame returned to the allocator.
.use_after_free:
    mov dword [USER_DATA_BASE + (mmap_arg - data_start) + 8], HAKU_PROT_READ | HAKU_PROT_WRITE
    call mmap_one_page
    mov edi, eax
    mov dword [edi], 0x33333333
    mov eax, SYS_MUNMAP
    mov ebx, edi
    mov ecx, 0x1000
    int 0x80
    test eax, eax
    jne fail
    mov dword [edi], 0x44444444
    jmp fail

mmap_one_page:
    mov eax, SYS_MMAP
    mov ebx, USER_DATA_BASE + (mmap_arg - data_start)
    int 0x80
    test eax, eax
    jle fail
    ret

fail:
    mov eax, SYS_EXIT
    mov ebx, 99
    int 0x80
.hang:
    jmp .hang
code_end:

    times 0x2000 - ($ - $$) db 0
data_start:
mode_byte:
    db 0
mmap_arg:
    dd 0, 0x1000, HAKU_PROT_READ | HAKU_PROT_WRITE, HAKU_MAP_PRIVATE | HAKU_MAP_ANONYMOUS, -1, 0
data_end:
