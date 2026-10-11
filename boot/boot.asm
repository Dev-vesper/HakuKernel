; =============================================================================
; MyOS v0.1 — boot sector (NASM, 16-bit real mode)
;
; BIOS loads these 512 bytes at 0x7C00 and jumps here with DL = boot drive.
; Steps: save drive -> load kernel (single-track read to 0x10000)
; -> enable A20 (Linux-style: test + bios/kbc/fast methods)
; -> load GDT -> enter 32-bit protected mode -> jump to KERNEL_ENTRY.
;
; A20 logic mirrors Linux arch/x86/boot/a20.c: after EVERY method we TEST
; the gate (write fs:0x200, read back via gs:0x210 which aliases it iff A20
; is off), instead of blindly assuming one method worked.
; KERNEL_ENTRY is passed by build.ps1 (-DKERNEL_ENTRY=...) from the real
; PE entry point; default keeps standalone assembly working.
; =============================================================================

[bits 16]
[org 0x7C00]

%ifndef KERNEL_ENTRY
%define KERNEL_ENTRY 0x10000
%endif

KERNEL_LOAD_SEG   equ 0x1000      ; kernel destination = 0x1000:0x0000
KERNEL_LOAD_OFF   equ 0x0000      ;   = linear 0x10000 (link.exe /BASE:0x10000)
KERNEL_SECTORS    equ 17          ; single floppy track (build.ps1 enforces it);
                                  ; the packed image (/ALIGN:16) must stay < 8.5 KiB

A20_TEST_SHORT equ 32
A20_TEST_LONG  equ 8192
A20_TRIES      equ 255

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00                ; stack grows down below the boot sector
    sti

    mov [boot_drive], dl          ; BIOS boot drive number — save it NOW

    mov si, msg_banner
    call print

    ; --- reset disk controller ------------------------------------------------
    xor ah, ah
    mov dl, [boot_drive]
    int 0x13
    jc disk_error

    ; --- load kernel: one int13 call, sectors 2..18 of track 0 ------------------
    ; (fits because KERNEL_SECTORS <= 17 < SECTORS_PER_TRACK; build.ps1 rejects
    ; bigger kernels with a pointer to extend this into a CHS loop)
    mov ax, KERNEL_LOAD_SEG
    mov es, ax
    mov bx, KERNEL_LOAD_OFF
    mov ah, 0x02                  ; read sectors
    mov al, KERNEL_SECTORS
    xor ch, ch                    ; track 0
    mov cl, 2                     ; from sector 2 (sector 1 is this boot sector)
    xor dh, dh                    ; head 0
    mov dl, [boot_drive]
    int 0x13
    jc disk_error
    cmp al, KERNEL_SECTORS        ; short read?
    jne disk_error

    ; --- enable A20, Linux-style (test after every method) --------------------
    call enable_a20
    cmp ax, 1
    jne a20_error
    mov si, msg_a20
    call print

    ; --- enter 32-bit protected mode ------------------------------------------
    cli
    lgdt [gdt_desc]
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp CODE_SEG:pm_start         ; far jump flushes the pipeline into 32-bit

; ---------------------------------------------------------------- print
; DS:SI -> zero-terminated string, BIOS teletype.
print:
    push ax
    push bx
    mov ah, 0x0E
    mov bx, 0x0007
.loop:
    lodsb
    test al, al
    jz .ret
    int 0x10
    jmp .loop
.ret:
    pop bx
    pop ax
    ret

fatal:
    cli
.hang:
    hlt
    jmp .hang

disk_error:
    mov si, msg_disk_err
    call print
    jmp fatal

a20_error:
    mov si, msg_a20_err
    call print
    jmp fatal

; ============================================================ A20 gate ======
; Mirrors Linux enable_a20(): test-short -> BIOS -> test-short -> drain KBC ->
; test-short -> KBC method -> test-long -> fast gate -> test-long, x255 tries.

; ax = a20_test(cx = iterations). Returns ax=1 gate open, 0 closed.
; Writes fs:0x200 (int 0x80 vector, saved+restored) and reads it back through
; gs:0x210; the two alias each other (xor == 0) exactly when A20 is off.
a20_test:
    push ebx
    push edx
    xor ax, ax
    mov fs, ax                    ; fs = 0x0000
    mov ax, 0FFFFh
    mov gs, ax                    ; gs = 0xFFFF
    mov ebx, [fs:0200h]           ; save int 0x80 vector
    mov edx, ebx                  ; ctr = saved
.loop:
    inc edx
    mov [fs:0200h], edx
    out 080h, al                  ; io_delay (POST card, like Linux)
    mov eax, [gs:0210h]
    xor eax, edx
    jnz .open                     ; mismatch => A20 is on
    loop .loop
    xor ax, ax                    ; still aliased => closed
    jmp .restore
.open:
    mov ax, 1
.restore:
    mov [fs:0200h], ebx
    pop edx
    pop ebx
    ret

; ax = empty_8042(). 0 = controller drained/ready, 1 = no KBC (or stuck).
; MAX loops with an 0xFF-status budget, like Linux (100000/32 -> 65536/32).
empty_8042:
    push bx
    push cx
    mov bx, 32                    ; 0xFF-status budget
    xor cx, cx                    ; 65536 polls
.poll:
    out 080h, al                  ; io_delay
    in al, 064h
    cmp al, 0FFh
    jne .bits
    dec bx
    jz .fail
.bits:
    test al, 01h
    jz .out_empty
    out 080h, al
    in al, 060h                   ; discard stale output data
    jmp .next
.out_empty:
    test al, 02h
    jnz .next                     ; input still busy -> keep waiting
    xor ax, ax                    ; both buffers empty -> ready
    pop cx
    pop bx
    ret
.next:
    loop .poll
.fail:
    mov ax, 1
    pop cx
    pop bx
    ret

a20_bios:
    mov ax, 02401h
    int 015h
    ret

a20_kbc:
    call empty_8042
    mov al, 0D1h
    out 064h, al                  ; command: write output port
    call empty_8042
    mov al, 0DFh
    out 060h, al                  ; A20 on
    call empty_8042
    mov al, 0FFh
    out 064h, al                  ; null command (UHCI wants it)
    call empty_8042
    ret

a20_fast:
    in al, 092h
    or al, 02h                    ; enable A20
    and al, 0FEh                  ; do NOT reset the machine
    out 092h, al
    ret

; ax = enable_a20(). 1 = open, 0 = failed after A20_TRIES rounds.
; Clobbers bx/cx (both dead at the single call site).
enable_a20:
    mov bx, A20_TRIES
.round:
    mov cx, A20_TEST_SHORT
    call a20_test                 ; already open? (modern BIOS/UEFI-CSM)
    cmp ax, 1
    je .ok

    call a20_bios
    mov cx, A20_TEST_SHORT
    call a20_test
    cmp ax, 1
    je .ok

    call empty_8042               ; ax = kbc present (0) or not
    push ax
    mov cx, A20_TEST_SHORT
    call a20_test                 ; BIOS may have worked with delay
    cmp ax, 1
    je .ok_pop
    pop ax
    cmp ax, 0
    jne .skip_kbc                 ; no controller -> skip its method
    call a20_kbc
    mov cx, A20_TEST_LONG         ; slow external circuitry needs patience
    call a20_test
    cmp ax, 1
    je .ok
.skip_kbc:
    call a20_fast
    mov cx, A20_TEST_LONG
    call a20_test
    cmp ax, 1
    je .ok

    dec bx
    jnz .round
    xor ax, ax                    ; FAILED
    ret
.ok_pop:
    pop ax                        ; drop saved kbc status
.ok:
    mov ax, 1
    ret

; ---------------------------------------------------------------- data
boot_drive:   db 0
msg_banner:   db 'MyOS v0.1', 13, 10, 0
msg_a20:      db 'A20 ok', 13, 10, 0
msg_disk_err: db 'disk err', 13, 10, 0
msg_a20_err:  db 'a20 err', 13, 10, 0

; ---------------------------------------------------------------- GDT (flat 4 GiB)
gdt_start:
    dq 0x0000000000000000          ; null descriptor
gdt_code:
    dw 0xFFFF                     ; limit 0:15
    dw 0x0000                     ; base 0:15
    db 0x00                       ; base 16:23
    db 1001_1010b                 ; present, ring 0, code, exec/read
    db 1100_1111b                 ; granularity 4K, 32-bit, limit 16:19 = 0xF
    db 0x00                       ; base 24:31
gdt_data:
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 1001_0010b                 ; present, ring 0, data, read/write
    db 1100_1111b
    db 0x00
gdt_end:

gdt_desc:
    dw gdt_end - gdt_start - 1
    dd gdt_start

CODE_SEG equ gdt_code - gdt_start ; 0x08
DATA_SEG equ gdt_data - gdt_start ; 0x10

; ---------------------------------------------------------------- 32-bit entry
[bits 32]
pm_start:
    mov ax, DATA_SEG
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x00090000           ; 576 KiB stack, safely below the EBDA
    jmp CODE_SEG:KERNEL_ENTRY     ; -> kernel entry point (from link.exe)

; ---------------------------------------------------------------- boot signature
times 510 - ($ - $$) db 0
dw 0xAA55
