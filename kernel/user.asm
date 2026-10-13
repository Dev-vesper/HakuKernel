; Minimal static ELF32/i386 executable used to exercise the ELF loader.
[bits 32]
[org 0]

%define USER_CODE_BASE 0x40000000
%define USER_DATA_BASE 0x40001000
%define USER_STACK_TOP 0xC0000000

%define SYS_EXIT   1
%define SYS_OPEN   5
%define SYS_CLOSE  6
%define SYS_READ   3
%define SYS_WRITE  4
%define SYS_UNLINK 10
%define SYS_LSEEK 19
%define SYS_GETPID 20
%define SYS_RENAME 38
%define SYS_DUP    41
%define SYS_DUP2   63
%define SYS_STAT   106
%define SYS_FSTAT  108

%define STAT_SIZE  8
%define STAT_MODE  0x81A4          ; S_IFREG | 0644

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

    ; Invalid open flag combinations and bad descriptors fail distinctly.
    mov esi, 30
    mov eax, SYS_OPEN
    mov ebx, USER_DATA_BASE + (file_path_one - data_start)
    mov ecx, 0x80              ; O_EXCL without O_CREAT
    xor edx, edx
    int 0x80
    cmp eax, -22               ; EINVAL
    jne .fail
    mov esi, 31
    mov eax, SYS_OPEN
    mov ebx, USER_DATA_BASE + (file_path_one - data_start)
    mov ecx, 0x200             ; O_TRUNC with O_RDONLY
    xor edx, edx
    int 0x80
    cmp eax, -22               ; EINVAL
    jne .fail
    mov esi, 32
    mov eax, SYS_CLOSE
    mov ebx, 99
    int 0x80
    cmp eax, -9                ; EBADF
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

    ; Create a per-process FAT12 file, write across a sector boundary,
    ; seek and read it back, then close and reopen it read-only.
    mov esi, 15
    mov eax, SYS_GETPID
    int 0x80
    cmp eax, 1
    je .file_pid_one
    mov ebx, USER_DATA_BASE + (file_path_two - data_start)
    jmp .file_open
.file_pid_one:
    mov ebx, USER_DATA_BASE + (file_path_one - data_start)
.file_open:
    mov [USER_DATA_BASE + (file_path_selected - data_start)], ebx
    mov esi, 15
    mov eax, SYS_OPEN
    xor ecx, ecx               ; O_RDONLY
    xor edx, edx
    int 0x80
    cmp eax, -2                ; ENOENT
    jne .fail
    mov ebx, [USER_DATA_BASE + (file_path_selected - data_start)]
    mov eax, SYS_OPEN
    mov ecx, 0x242             ; O_CREAT | O_RDWR | O_TRUNC
    xor edx, edx
    int 0x80
    cmp eax, 3
    jb .fail
    mov [USER_DATA_BASE + (file_fd - data_start)], eax
    mov edi, eax               ; retain descriptor across syscalls
    mov esi, 16
    mov eax, SYS_WRITE
    mov ebx, edi
    mov ecx, USER_DATA_BASE + (file_payload - data_start)
    mov edx, file_payload_end - file_payload
    int 0x80
    cmp eax, file_payload_end - file_payload
    jne .fail

    mov esi, 33
    mov eax, SYS_LSEEK
    mov ebx, edi               ; valid descriptor, invalid whence
    xor ecx, ecx
    mov edx, 99
    int 0x80
    cmp eax, -22               ; EINVAL, position unchanged
    jne .fail

    mov esi, 17
    mov eax, SYS_OPEN
    mov ebx, [USER_DATA_BASE + (file_path_selected - data_start)]
    mov ecx, 0x0C2             ; O_CREAT | O_EXCL | O_RDWR
    xor edx, edx
    int 0x80
    cmp eax, -17               ; EEXIST
    jne .fail
    mov eax, SYS_OPEN
    mov ebx, [USER_DATA_BASE + (file_path_selected - data_start)]
    mov ecx, 1                 ; O_WRONLY
    xor edx, edx
    int 0x80
    cmp eax, 3
    jb .fail
    mov edi, eax
    mov eax, SYS_READ
    mov ebx, edi
    mov ecx, USER_DATA_BASE + (file_readback - data_start)
    mov edx, 1
    int 0x80
    cmp eax, -9                ; EBADF for a write-only descriptor
    jne .fail
    mov eax, SYS_CLOSE
    mov ebx, edi
    int 0x80
    test eax, eax
    jne .fail

    mov eax, SYS_OPEN
    mov ebx, [USER_DATA_BASE + (file_path_selected - data_start)]
    mov ecx, 0x401             ; O_WRONLY | O_APPEND
    xor edx, edx
    int 0x80
    cmp eax, 3
    jb .fail
    mov edi, eax
    mov esi, 18
    mov eax, SYS_WRITE
    mov ebx, edi
    mov ecx, USER_DATA_BASE + (append_payload - data_start)
    mov edx, 1
    int 0x80
    cmp eax, 1
    jne .fail
    mov eax, SYS_LSEEK
    mov ebx, edi
    xor ecx, ecx
    mov edx, 2                 ; SEEK_END
    int 0x80
    cmp eax, file_payload_end - file_payload + 1
    jne .fail
    mov eax, SYS_CLOSE
    mov ebx, edi
    int 0x80
    test eax, eax
    jne .fail

    mov edi, [USER_DATA_BASE + (file_fd - data_start)]
    mov esi, 19
    mov eax, SYS_LSEEK
    mov ebx, edi
    xor ecx, ecx
    xor edx, edx               ; SEEK_SET
    int 0x80
    test eax, eax
    jne .fail
    mov esi, 20
    mov eax, SYS_READ
    mov ebx, edi
    mov ecx, USER_DATA_BASE + (file_readback - data_start)
    mov edx, file_payload_end - file_payload
    int 0x80
    cmp eax, file_payload_end - file_payload
    jne .fail
    push esi
    push edi
    mov esi, USER_DATA_BASE + (file_payload - data_start)
    mov edi, USER_DATA_BASE + (file_readback - data_start)
    mov ecx, file_payload_end - file_payload
    cld
    repe cmpsb
    pop edi
    pop esi
    jne .fail

    mov esi, 21
    mov eax, SYS_LSEEK
    mov ebx, edi
    xor ecx, ecx
    mov edx, 2                 ; SEEK_END
    int 0x80
    cmp eax, file_payload_end - file_payload + 1
    jne .fail
    mov esi, 22
    mov eax, SYS_LSEEK
    mov ebx, edi
    mov ecx, -1
    mov edx, 2                 ; seek to appended byte
    int 0x80
    cmp eax, file_payload_end - file_payload
    jne .fail
    mov eax, SYS_READ
    mov ebx, edi
    mov ecx, USER_DATA_BASE + (file_readback - data_start)
    mov edx, 1
    int 0x80
    cmp eax, 1
    jne .fail
    cmp byte [USER_DATA_BASE + (file_readback - data_start)], 'B'
    jne .fail
    mov esi, 23
    mov eax, SYS_CLOSE
    mov ebx, edi
    int 0x80
    test eax, eax
    jne .fail
    mov esi, 24
    mov eax, SYS_OPEN
    mov ebx, [USER_DATA_BASE + (file_path_selected - data_start)]
    xor ecx, ecx               ; O_RDONLY
    xor edx, edx
    int 0x80
    cmp eax, 3
    jb .fail
    mov edi, eax
    mov esi, 24
    mov eax, SYS_READ
    mov ebx, edi
    mov ecx, USER_DATA_BASE + (file_readback - data_start)
    mov edx, 1
    int 0x80
    cmp eax, 1
    jne .fail
    cmp byte [USER_DATA_BASE + (file_readback - data_start)], 'A'
    jne .fail
    mov eax, SYS_WRITE
    mov ebx, edi
    mov ecx, USER_DATA_BASE + (append_payload - data_start)
    mov edx, 1
    int 0x80
    cmp eax, -9                ; EBADF for a read-only descriptor
    jne .fail
    mov esi, 25
    mov eax, SYS_CLOSE
    mov ebx, edi
    int 0x80
    test eax, eax
    jne .fail
    mov esi, 21
    mov eax, SYS_OPEN
    mov ebx, [USER_DATA_BASE + (file_path_selected - data_start)]
    xor ecx, ecx               ; O_RDONLY
    xor edx, edx
    int 0x80
    cmp eax, 3
    jb .fail
    mov edi, eax
    mov esi, 22
    mov eax, SYS_READ
    mov ebx, edi
    mov ecx, USER_DATA_BASE + (file_readback - data_start)
    mov edx, 4
    int 0x80
    cmp eax, 4
    jne .fail
    push esi
    push edi
    mov esi, USER_DATA_BASE + (file_payload - data_start)
    mov edi, USER_DATA_BASE + (file_readback - data_start)
    mov ecx, 4
    cld
    repe cmpsb
    pop edi
    pop esi
    jne .fail
    mov esi, 23
    mov eax, SYS_CLOSE
    mov ebx, edi
    int 0x80
    test eax, eax
    jne .fail

    mov esi, 24
    mov eax, SYS_WRITE
    mov ebx, 1
    mov ecx, USER_DATA_BASE + (file_io_message - data_start)
    mov edx, file_io_message_end - file_io_message
    int 0x80
    cmp eax, edx
    jne .fail

    ; unlink/rename/stat/fstat, including open files that are removed or
    ; renamed. Per-process names (patched with the pid digit) so the two
    ; instances never collide; shared Q-names are never created.
    mov esi, 34
    mov eax, SYS_GETPID
    int 0x80
    add eax, '0'                   ; '1' or '2' (range proven above)
    mov [USER_DATA_BASE + (unlink_path - data_start) + 1], al
    mov [USER_DATA_BASE + (rename_src - data_start) + 1], al
    mov [USER_DATA_BASE + (rename_dst - data_start) + 1], al
    mov [USER_DATA_BASE + (rename_dst2 - data_start) + 1], al
    mov [USER_DATA_BASE + (extra_path - data_start) + 1], al
    mov ebx, USER_DATA_BASE + (unlink_path - data_start)
    mov [USER_DATA_BASE + (ops_unlink - data_start)], ebx
    mov ebx, USER_DATA_BASE + (rename_src - data_start)
    mov [USER_DATA_BASE + (ops_src - data_start)], ebx
    mov ebx, USER_DATA_BASE + (rename_dst - data_start)
    mov [USER_DATA_BASE + (ops_dst - data_start)], ebx
    mov ebx, USER_DATA_BASE + (rename_dst2 - data_start)
    mov [USER_DATA_BASE + (ops_dst2 - data_start)], ebx
    mov ebx, USER_DATA_BASE + (extra_path - data_start)
    mov [USER_DATA_BASE + (ops_extra - data_start)], ebx
    ; stat on a missing name -> ENOENT
    mov eax, SYS_STAT
    mov ebx, [USER_DATA_BASE + (ops_unlink - data_start)]
    mov ecx, USER_DATA_BASE + (stat_buf - data_start)
    int 0x80
    cmp eax, -2
    jne .fail
    ; fstat on a bad descriptor -> EBADF, stat to nowhere -> EFAULT
    mov esi, 35
    mov eax, SYS_FSTAT
    mov ebx, 99
    mov ecx, USER_DATA_BASE + (stat_buf - data_start)
    int 0x80
    cmp eax, -9
    jne .fail
    mov eax, SYS_STAT
    mov ebx, [USER_DATA_BASE + (ops_unlink - data_start)]
    mov ecx, 0x1000                ; unmapped
    int 0x80
    cmp eax, -14
    jne .fail

    ; create, then stat size+mode and fstat the open fd
    mov esi, 36
    mov eax, SYS_OPEN
    mov ebx, [USER_DATA_BASE + (ops_unlink - data_start)]
    mov ecx, 0x242               ; O_CREAT | O_RDWR | O_TRUNC
    xor edx, edx
    int 0x80
    cmp eax, 3
    jb .fail
    mov edi, eax
    mov eax, SYS_WRITE
    mov ebx, edi
    mov ecx, USER_DATA_BASE + (file_payload - data_start)
    mov edx, 10
    int 0x80
    cmp eax, 10
    jne .fail
    mov eax, SYS_CLOSE
    mov ebx, edi
    int 0x80
    test eax, eax
    jne .fail
    mov eax, SYS_STAT
    mov ebx, [USER_DATA_BASE + (ops_unlink - data_start)]
    mov ecx, USER_DATA_BASE + (stat_buf - data_start)
    int 0x80
    test eax, eax
    jne .fail
    cmp dword [USER_DATA_BASE + (stat_buf - data_start)], 10
    jne .fail
    cmp dword [USER_DATA_BASE + (stat_buf - data_start) + 4], STAT_MODE
    jne .fail
    mov eax, SYS_OPEN
    mov ebx, [USER_DATA_BASE + (ops_unlink - data_start)]
    mov ecx, 2                   ; O_RDWR
    xor edx, edx
    int 0x80
    cmp eax, 3
    jb .fail
    mov edi, eax
    mov eax, SYS_FSTAT
    mov ebx, edi
    mov ecx, USER_DATA_BASE + (stat_buf - data_start)
    int 0x80
    test eax, eax
    jne .fail
    cmp dword [USER_DATA_BASE + (stat_buf - data_start)], 10
    jne .fail
    mov eax, SYS_CLOSE
    mov ebx, edi
    int 0x80
    test eax, eax
    jne .fail

    ; unlink: the name and its stat entry die together
    mov esi, 37
    mov eax, SYS_UNLINK
    mov ebx, [USER_DATA_BASE + (ops_unlink - data_start)]
    int 0x80
    test eax, eax
    jne .fail
    mov eax, SYS_OPEN
    mov ebx, [USER_DATA_BASE + (ops_unlink - data_start)]
    xor ecx, ecx
    xor edx, edx
    int 0x80
    cmp eax, -2
    jne .fail
    mov eax, SYS_STAT
    mov ebx, [USER_DATA_BASE + (ops_unlink - data_start)]
    mov ecx, USER_DATA_BASE + (stat_buf - data_start)
    int 0x80
    cmp eax, -2
    jne .fail

    ; removed-but-open: the descriptor keeps working until close
    mov esi, 38
    mov eax, SYS_OPEN
    mov ebx, [USER_DATA_BASE + (ops_unlink - data_start)]
    mov ecx, 0x242
    xor edx, edx
    int 0x80
    cmp eax, 3
    jb .fail
    mov edi, eax
    mov eax, SYS_WRITE
    mov ebx, edi
    mov ecx, USER_DATA_BASE + (file_payload - data_start)
    mov edx, 10
    int 0x80
    cmp eax, 10
    jne .fail
    mov eax, SYS_UNLINK
    mov ebx, [USER_DATA_BASE + (ops_unlink - data_start)]
    int 0x80
    test eax, eax
    jne .fail
    mov eax, SYS_LSEEK            ; rewind: overwrite, not append
    mov ebx, edi
    xor ecx, ecx
    xor edx, edx
    int 0x80
    test eax, eax
    jne .fail
    mov eax, SYS_WRITE            ; overwrite 5 through the open fd
    mov ebx, edi
    mov ecx, USER_DATA_BASE + (file_payload - data_start)
    mov edx, 5
    int 0x80
    cmp eax, 5
    jne .fail
    mov eax, SYS_LSEEK
    mov ebx, edi
    xor ecx, ecx
    xor edx, edx
    int 0x80
    test eax, eax
    jne .fail
    mov eax, SYS_READ
    mov ebx, edi
    mov ecx, USER_DATA_BASE + (file_readback - data_start)
    mov edx, 5
    int 0x80
    cmp eax, 5
    jne .fail
    push esi
    push edi
    mov esi, USER_DATA_BASE + (file_payload - data_start)
    mov edi, USER_DATA_BASE + (file_readback - data_start)
    mov ecx, 5
    cld
    repe cmpsb
    pop edi
    pop esi
    jne .fail
    mov eax, SYS_FSTAT            ; size still 10: overwrite, no extend
    mov ebx, edi
    mov ecx, USER_DATA_BASE + (stat_buf - data_start)
    int 0x80
    test eax, eax
    jne .fail
    cmp dword [USER_DATA_BASE + (stat_buf - data_start)], 10
    jne .fail
    mov eax, SYS_CLOSE
    mov ebx, edi
    int 0x80
    test eax, eax
    jne .fail
    mov eax, SYS_OPEN            ; name stayed dead after close
    mov ebx, [USER_DATA_BASE + (ops_unlink - data_start)]
    xor ecx, ecx
    xor edx, edx
    int 0x80
    cmp eax, -2
    jne .fail

    ; rename: old name dies, new name serves the same bytes
    mov esi, 39
    mov eax, SYS_OPEN
    mov ebx, [USER_DATA_BASE + (ops_src - data_start)]
    mov ecx, 0x242
    xor edx, edx
    int 0x80
    cmp eax, 3
    jb .fail
    mov edi, eax
    mov eax, SYS_WRITE
    mov ebx, edi
    mov ecx, USER_DATA_BASE + (file_payload - data_start)
    mov edx, 8
    int 0x80
    cmp eax, 8
    jne .fail
    mov eax, SYS_CLOSE
    mov ebx, edi
    int 0x80
    test eax, eax
    jne .fail
    mov eax, SYS_RENAME
    mov ebx, [USER_DATA_BASE + (ops_src - data_start)]
    mov ecx, [USER_DATA_BASE + (ops_dst - data_start)]
    int 0x80
    test eax, eax
    jne .fail
    mov eax, SYS_OPEN
    mov ebx, [USER_DATA_BASE + (ops_src - data_start)]
    xor ecx, ecx
    xor edx, edx
    int 0x80
    cmp eax, -2
    jne .fail
    mov eax, SYS_STAT
    mov ebx, [USER_DATA_BASE + (ops_src - data_start)]
    mov ecx, USER_DATA_BASE + (stat_buf - data_start)
    int 0x80
    cmp eax, -2
    jne .fail
    mov eax, SYS_STAT
    mov ebx, [USER_DATA_BASE + (ops_dst - data_start)]
    mov ecx, USER_DATA_BASE + (stat_buf - data_start)
    int 0x80
    test eax, eax
    jne .fail
    cmp dword [USER_DATA_BASE + (stat_buf - data_start)], 8
    jne .fail

    ; renamed-but-open: the old descriptor follows the entry
    mov esi, 40
    mov eax, SYS_OPEN
    mov ebx, [USER_DATA_BASE + (ops_dst - data_start)]
    mov ecx, 2
    xor edx, edx
    int 0x80
    cmp eax, 3
    jb .fail
    mov edi, eax
    mov eax, SYS_RENAME
    mov ebx, [USER_DATA_BASE + (ops_dst - data_start)]
    mov ecx, [USER_DATA_BASE + (ops_dst2 - data_start)]
    int 0x80
    test eax, eax
    jne .fail
    mov eax, SYS_LSEEK
    mov ebx, edi
    xor ecx, ecx
    xor edx, edx
    int 0x80
    test eax, eax
    jne .fail
    mov eax, SYS_READ
    mov ebx, edi
    mov ecx, USER_DATA_BASE + (file_readback - data_start)
    mov edx, 8
    int 0x80
    cmp eax, 8
    jne .fail
    push esi
    push edi
    mov esi, USER_DATA_BASE + (file_payload - data_start)
    mov edi, USER_DATA_BASE + (file_readback - data_start)
    mov ecx, 8
    cld
    repe cmpsb
    pop edi
    pop esi
    jne .fail
    mov eax, SYS_OPEN
    mov ebx, [USER_DATA_BASE + (ops_dst2 - data_start)]
    xor ecx, ecx
    xor edx, edx
    int 0x80
    cmp eax, 3
    jb .fail
    mov [USER_DATA_BASE + (ops_fd - data_start)], eax
    mov eax, SYS_READ
    mov ebx, [USER_DATA_BASE + (ops_fd - data_start)]
    mov ecx, USER_DATA_BASE + (file_readback - data_start)
    mov edx, 8
    int 0x80
    cmp eax, 8
    jne .fail
    push esi
    push edi
    mov esi, USER_DATA_BASE + (file_payload - data_start)
    mov edi, USER_DATA_BASE + (file_readback - data_start)
    mov ecx, 8
    cld
    repe cmpsb
    pop edi
    pop esi
    jne .fail
    mov eax, SYS_CLOSE
    mov ebx, [USER_DATA_BASE + (ops_fd - data_start)]
    int 0x80
    test eax, eax
    jne .fail
    mov eax, SYS_CLOSE
    mov ebx, edi
    int 0x80
    test eax, eax
    jne .fail
    mov eax, SYS_OPEN            ; intermediate name stayed dead
    mov ebx, [USER_DATA_BASE + (ops_dst - data_start)]
    xor ecx, ecx
    xor edx, edx
    int 0x80
    cmp eax, -2
    jne .fail

    mov esi, 41
    mov eax, SYS_RENAME          ; missing source -> ENOENT
    mov ebx, USER_DATA_BASE + (missing_q9 - data_start)
    mov ecx, USER_DATA_BASE + (missing_q8 - data_start)
    int 0x80
    cmp eax, -2
    jne .fail
    mov eax, SYS_RENAME          ; same name -> no-op success
    mov ebx, [USER_DATA_BASE + (ops_dst2 - data_start)]
    mov ecx, [USER_DATA_BASE + (ops_dst2 - data_start)]
    int 0x80
    test eax, eax
    jne .fail
    mov eax, SYS_OPEN            ; fresh file for the EEXIST probe
    mov ebx, [USER_DATA_BASE + (ops_extra - data_start)]
    mov ecx, 0x242
    xor edx, edx
    int 0x80
    cmp eax, 3
    jb .fail
    mov edi, eax
    mov eax, SYS_CLOSE
    mov ebx, edi
    int 0x80
    test eax, eax
    jne .fail
    mov eax, SYS_RENAME          ; onto an existing name -> EEXIST
    mov ebx, [USER_DATA_BASE + (ops_extra - data_start)]
    mov ecx, [USER_DATA_BASE + (ops_dst2 - data_start)]
    int 0x80
    cmp eax, -17
    jne .fail
    mov eax, SYS_UNLINK          ; missing name -> ENOENT
    mov ebx, USER_DATA_BASE + (missing_q9 - data_start)
    int 0x80
    cmp eax, -2
    jne .fail

    mov esi, 42
    mov eax, SYS_WRITE
    mov ebx, 1
    mov ecx, USER_DATA_BASE + (file_ops_message - data_start)
    mov edx, file_ops_message_end - file_ops_message
    int 0x80
    cmp eax, edx
    jne .fail

    ; dup/dup2: descriptors may share one open file description (offset,
    ; access mode, and a refcount that keeps the file alive until the last
    ; of them closes). Per-process D-names avoid collisions between the two
    ; instances.
    mov esi, 50
    mov eax, SYS_GETPID
    int 0x80
    add eax, '0'
    mov [USER_DATA_BASE + (dup_path - data_start) + 1], al
    mov eax, SYS_OPEN
    mov ebx, USER_DATA_BASE + (dup_path - data_start)
    mov ecx, 0x242             ; O_CREAT | O_RDWR | O_TRUNC
    xor edx, edx
    int 0x80
    cmp eax, 3
    jb .fail
    mov edi, eax               ; fd_a
    mov eax, SYS_WRITE
    mov ebx, edi
    mov ecx, USER_DATA_BASE + (file_payload - data_start)
    mov edx, 4
    int 0x80
    cmp eax, 4
    jne .fail

    ; dup -> lowest free fd, distinct from the original
    mov esi, 51
    mov eax, SYS_DUP
    mov ebx, edi
    int 0x80
    cmp eax, 3
    jb .fail
    cmp eax, edi
    je .fail
    mov [USER_DATA_BASE + (dup_fd_b - data_start)], eax

    ; shared offset: rewind through fd_a, fd_b reads from the same position
    mov esi, 52
    mov eax, SYS_LSEEK
    mov ebx, edi
    xor ecx, ecx
    xor edx, edx               ; SEEK_SET
    int 0x80
    test eax, eax
    jne .fail
    mov eax, SYS_READ
    mov ebx, [USER_DATA_BASE + (dup_fd_b - data_start)]
    mov ecx, USER_DATA_BASE + (dup_buf - data_start)
    mov edx, 4
    int 0x80
    cmp eax, 4
    jne .fail
    cmp byte [USER_DATA_BASE + (dup_buf - data_start)], 'A'
    jne .fail

    ; dup2 onto a chosen free fd returns that fd
    mov esi, 53
    mov eax, SYS_DUP2
    mov ebx, edi
    mov ecx, 5
    int 0x80
    cmp eax, 5
    jne .fail

    ; dup2(fd, fd) is a no-op that returns fd
    mov esi, 54
    mov eax, SYS_DUP2
    mov ebx, edi
    mov ecx, edi
    int 0x80
    cmp eax, edi
    jne .fail

    ; dup and dup2 with an invalid source -> EBADF (target left intact)
    mov esi, 55
    mov eax, SYS_DUP
    mov ebx, 99
    int 0x80
    cmp eax, -9
    jne .fail
    mov esi, 56
    mov eax, SYS_DUP2
    mov ebx, 99
    mov ecx, 5
    int 0x80
    cmp eax, -9
    jne .fail

    ; closing the original keeps the duplicates usable (refcount)
    mov esi, 57
    mov eax, SYS_CLOSE
    mov ebx, edi
    int 0x80
    test eax, eax
    jne .fail
    mov eax, SYS_LSEEK
    mov ebx, 5
    xor ecx, ecx
    xor edx, edx
    int 0x80
    test eax, eax
    jne .fail
    mov esi, 58
    mov eax, SYS_READ
    mov ebx, [USER_DATA_BASE + (dup_fd_b - data_start)]
    mov ecx, USER_DATA_BASE + (dup_buf - data_start)
    mov edx, 4
    int 0x80
    cmp eax, 4
    jne .fail
    cmp byte [USER_DATA_BASE + (dup_buf - data_start)], 'A'
    jne .fail
    mov eax, SYS_CLOSE
    mov ebx, 5
    int 0x80
    test eax, eax
    jne .fail
    mov eax, SYS_CLOSE
    mov ebx, [USER_DATA_BASE + (dup_fd_b - data_start)]
    int 0x80
    test eax, eax
    jne .fail

    mov esi, 59
    mov eax, SYS_WRITE
    mov ebx, 1
    mov ecx, USER_DATA_BASE + (dup_message - data_start)
    mov edx, dup_message_end - dup_message
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
    db "USERMODE-OK ELF32 RX/RW + POSIX-style file syscalls", 10
startup_message_end:
read_message:
    db "USER-READ-OK ELF data and stack copy_to_user", 10
read_message_end:
file_io_message:
    db "USER-FILE-IO-OK create/write/read/lseek/close", 10
file_io_message_end:
checks_message:
    db "USER-SYSCALL-CHECKS-OK", 10
checks_message_end:
failure_message:
    db "USER-SYSCALL-FAIL", 10
failure_message_end:
file_path_one:
    db "P1.TXT", 0
file_path_two:
    db "P2.TXT", 0
file_path_selected:
    dd 0
file_fd:
    dd 0
file_ops_message:
    db "USER-FILE-OPS-OK unlink/rename/stat/fstat", 10
file_ops_message_end:
unlink_path:
    db "U0.TXT", 0
rename_src:
    db "R0A.TXT", 0
rename_dst:
    db "R0B.TXT", 0
rename_dst2:
    db "R0C.TXT", 0
extra_path:
    db "X0.TXT", 0
missing_q9:
    db "Q9.TXT", 0
missing_q8:
    db "Q8.TXT", 0
ops_unlink:
    dd 0
ops_src:
    dd 0
ops_dst:
    dd 0
ops_dst2:
    dd 0
ops_extra:
    dd 0
ops_fd:
    dd 0
dup_message:
    db "USER-DUP-OK dup/dup2 shared file offset", 10
dup_message_end:
dup_path:
    db "D0.TXT", 0
dup_fd_b:
    dd 0
dup_buf:
    times 4 db 0
stat_buf:
    times 8 db 0
append_payload:
    db 'B'
file_payload:
    times 700 db 'A'
file_payload_end:
file_readback:
    times file_payload_end - file_payload db 0
data_file_end:
    times 32 db 0
bss_word:
    dd 0
data_end:
