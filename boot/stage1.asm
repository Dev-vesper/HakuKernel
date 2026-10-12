; =============================================================================
; Haku stage 1 — boot sector (NASM, 16-bit, must stay within 510 bytes)
;
; BIOS loads this at 0x7C00 (DL = boot drive). It loads the stage-2 loader
; (STAGE2_SECTORS sectors right after this one) to 0x7E00 and jumps there.
; Only one disk read, single track — deliberately tiny so it always fits.
; =============================================================================

[bits 16]
[org 0x7C00]

STAGE2_SECTORS equ 8
STAGE2_SEG     equ 0x07E0          ; 0x07E0:0x0000 = linear 0x7E00

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti

    mov [boot_drive], dl

    mov si, msg_boot
    call print

    ; Stash the drive in low scratch RAM too: BIOS calls below may clobber
    ; DL, and stage 2 (loaded later) must read the drive from somewhere
    ; reliable. 0x500 is free conventional memory (past the BDA).
    mov [0x500], dl

    call serial_init              ; COM1 for headless boot telemetry
    mov al, 'S'
    call serial_putc
    mov al, '1'
    call serial_putc
    mov al, 10
    call serial_putc

    xor ah, ah                    ; reset disk controller
    mov dl, [boot_drive]
    int 0x13
    jc disk_error

    mov ax, STAGE2_SEG            ; load stage 2 to 0x7E00
    mov es, ax
    xor bx, bx
    mov ah, 0x02
    mov al, STAGE2_SECTORS
    xor ch, ch                    ; track 0
    mov cl, 2                     ; sectors 2..9 (sector 1 is this boot sector)
    xor dh, dh                    ; head 0
    mov dl, [boot_drive]
    int 0x13
    jc disk_error
    cmp al, STAGE2_SECTORS        ; short read?
    jne disk_error

    jmp 0x0000:0x7E00             ; -> stage 2

print:                            ; DS:SI -> zero-terminated string
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

disk_error:
    mov si, msg_disk_err
    call print
    cli
.hang:
    hlt
    jmp .hang

; ---------------------------------------------------------- COM1 out
; Minimal polled serial for boot telemetry (visible with -serial file:).
serial_init:
    mov dx, 0x3F9
    xor al, al
    out dx, al                    ; disable interrupts
    mov dx, 0x3FB
    mov al, 0x80
    out dx, al                    ; enable divisor latch
    mov dx, 0x3F8
    mov al, 0x03
    out dx, al                    ; 38400 baud
    mov dx, 0x3F9
    xor al, al
    out dx, al
    mov dx, 0x3FB
    mov al, 0x03
    out dx, al                    ; 8N1
    mov dx, 0x3FA
    mov al, 0xC7
    out dx, al                    ; FIFO on
    mov dx, 0x3FC
    mov al, 0x0B
    out dx, al                    ; RTS+DTR
    ret

serial_putc:                      ; AL = char
    push dx
    mov ah, al
.wait:
    mov dx, 0x3FD
    in al, dx
    test al, 0x20
    jz .wait
    mov dx, 0x3F8
    mov al, ah
    out dx, al
    pop dx
    ret

boot_drive:   db 0
msg_boot:     db 'Haku', 13, 10, 0
msg_disk_err: db 'stage1 disk err', 13, 10, 0

times 510 - ($ - $$) db 0
dw 0xAA55
