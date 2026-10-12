; =============================================================================
; Haku stage 2 — second-stage loader (NASM, 16-bit, loaded at 0x7E00)
;
; Loaded by stage 1 (8 sectors, up to 4 KiB).
; Steps: load kernel (track-safe CHS loop) to 0x10000 -> enable A20
; (Linux-style: test after BIOS/KBC/fast methods) -> load GDT ->
; enter 32-bit protected mode -> jump to KERNEL_ENTRY.
;
; Disk layout (see mkimg.py): [stage1: 1 sector][stage2: 8][kernel].
; So the kernel starts at LBA 9 = track 0, sector 10.
; A20 logic mirrors Linux arch/x86/boot/a20.c.
; KERNEL_ENTRY is passed by build.py (-DKERNEL_ENTRY=...) from the real
; PE entry point; default keeps standalone assembly working.
; =============================================================================

[bits 16]
[org 0x7E00]

%ifndef KERNEL_ENTRY
%define KERNEL_ENTRY 0x10000
%endif

KERNEL_LOAD_SEG    equ 0x1000     ; kernel destination = 0x1000:0x0000
KERNEL_LOAD_OFF    equ 0x0000     ;   = linear 0x10000 (link.exe /BASE:0x10000)
%ifndef KERNEL_SECTORS
%define KERNEL_SECTORS 896        ; maximum 448 KiB, ending below 0x80000
%endif
KERNEL_MAX_SECTORS equ 896        ; leave 64 KiB below the bootstrap stack
KERNEL_FIRST_SECTOR equ 10        ; 1 + 8 stage-2 sectors + 1 (mkimg cross-checks)
SECTORS_PER_TRACK  equ 18
HEADS              equ 2

A20_TEST_SHORT equ 32
A20_TEST_LONG  equ 8192
A20_TRIES      equ 255

stage2:
    ; DS/ES/SS/SP survive from stage 1. DL may NOT (BIOS calls clobber it),
    ; so the boot drive comes from the scratch cell stage 1 stashed at 0x500.
    mov dl, [0x500]
    mov [boot_drive], dl

    mov si, msg_loading
    call print

    mov al, 'S'                   ; headless telemetry (COM1 is already up)
    call serial_putc
    mov al, '2'
    call serial_putc
    mov al, 10
    call serial_putc

    xor ah, ah                    ; reset disk controller
    mov dl, [boot_drive]
    int 0x13
    jc disk_error

    ; --- load kernel with track-safe INT 13h AH=0x02 reads --------------------
    ; Read one track fragment at a time; each request stays within a track
    ; and under the BIOS 64 KiB transfer boundary. ES advances by 32
    ; paragraphs per sector, so the destination can grow beyond 64 KiB.
    mov word [kernel_sectors_left], KERNEL_SECTORS
    mov word [kernel_dest_segment], KERNEL_LOAD_SEG
    mov byte [kernel_sector], KERNEL_FIRST_SECTOR
    mov byte [kernel_head], 0
    mov byte [kernel_cylinder], 0
.kernel_read_loop:
    cmp word [kernel_sectors_left], 0
    je .kernel_read_done

    mov al, SECTORS_PER_TRACK + 1
    sub al, [kernel_sector]      ; sectors left in this track
    xor ah, ah
    mov bx, ax
    mov ax, [kernel_sectors_left]
    cmp ax, bx
    jae .kernel_chunk_ready
    mov bx, ax                   ; final request may be shorter than a track
.kernel_chunk_ready:
    mov [kernel_chunk], bl
    mov ax, [kernel_dest_segment]
    mov es, ax
    xor bx, bx                   ; request is at most one 9 KiB track
    mov ah, 0x02
    mov al, [kernel_chunk]
    mov ch, [kernel_cylinder]
    mov cl, [kernel_sector]
    mov dh, [kernel_head]
    mov dl, [boot_drive]
    push ds
    int 0x13
    pop ds
    jc disk_error

    xor ax, ax
    mov al, [kernel_chunk]
    sub [kernel_sectors_left], ax
    shl ax, 5                    ; 512 bytes = 32 paragraphs
    add [kernel_dest_segment], ax

    mov byte [kernel_sector], 1
    cmp byte [kernel_head], HEADS - 1
    jb .kernel_next_head
    mov byte [kernel_head], 0
    inc byte [kernel_cylinder]
    jmp .kernel_read_loop
.kernel_next_head:
    inc byte [kernel_head]
    jmp .kernel_read_loop
.kernel_read_done:

    ; debug – read loop finished
    mov al, 'R'
    call serial_putc

    mov al, 'K'                   ; kernel sectors landed
    call serial_putc
    mov al, 10
    call serial_putc

    mov si, msg_loaded
    call print

    ; --- collect E820 memory map (INT 15h AX=E820h) ---------------------------
    ; Must happen in real mode (BIOS). Entries land at 0x7000 (24 B each,
    ; up to 24 entries = 576 B). Count stored at 0x6FFC (dword).
    ; Layout per entry: base(8) + length(8) + type(4) + acpi(4) = 24 bytes.
    ; type 1 = usable RAM, 2 = reserved, 3 = ACPI reclaimable, 5 = bad.
    xor ax, ax
    mov es, ax                  ; ES:DI = 0x0000:0x7000 = physical 0x7000
    mov di, 0x7000
    xor ebx, ebx                ; continuation value (0 = first call)
    xor ebp, ebp                ; entry counter
.e820_loop:
    cmp ebp, 24                 ; max entries (fits below 0x7C00)
    jae .e820_done
    mov eax, 0xE820
    mov edx, 0x534D4150         ; 'SMAP' — BIOS checks this
    mov ecx, 24                 ; buffer size per entry
    mov dword [di + 20], 1      ; ACPI 3.0: force "valid" attribute
    int 0x15
    jc .e820_done               ; CF set = error or no more entries
    cmp eax, 0x534D4150         ; BIOS must echo 'SMAP' in EAX
    jne .e820_done
    inc ebp
    add di, 24
    test ebx, ebx               ; EBX = 0 means last entry
    jnz .e820_loop
.e820_done:
    mov [0x6FFC], ebp           ; store entry count for the kernel

    ; --- enable A20, Linux-style (test after every method) --------------------
    call enable_a20
    cmp ax, 1
    jne a20_error
    mov al, 'A'                   ; A20 gate verified open
    call serial_putc
    mov al, 10
    call serial_putc
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

; ---------------------------------------------------------- COM1 out
; Polled serial_putc for headless telemetry (COM1 was inited by stage 1).
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
; Bounded polls with an 0xFF-status budget, like Linux (100000/32).
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
kernel_sector: db 0
kernel_head: db 0
kernel_cylinder: db 0
kernel_chunk: db 0
kernel_sectors_left: dw 0
kernel_dest_segment: dw 0
msg_loading:  db 'Haku stage2: loading kernel...', 13, 10, 0
msg_loaded:   db 'Haku stage2: kernel at 0x10000', 13, 10, 0
msg_a20:      db 'Haku stage2: A20 gate open (tested)', 13, 10, 0
msg_disk_err: db 'Haku stage2: DISK READ ERROR', 13, 10, 0
msg_a20_err:  db 'Haku stage2: A20 FAILED (tried BIOS+KBC+fast)', 13, 10, 0

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
