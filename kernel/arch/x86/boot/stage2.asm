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
    ; BIOS CHS reads may not cross a 64 KiB DMA boundary. Limit the
    ; track-sized request using the destination's low 16-bit address.
    mov ax, [kernel_dest_segment]
    shl ax, 4
    neg ax
    jnz .dma_boundary_distance
    mov ax, 128                  ; already aligned: one full 64 KiB window
    jmp .kernel_dma_limit_ready
.dma_boundary_distance:
    shr ax, 9                    ; sectors remaining before the boundary
.kernel_dma_limit_ready:
    cmp bx, ax
    jbe .kernel_dma_chunk_ready
    mov bx, ax
.kernel_dma_chunk_ready:
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

    ; A DMA-boundary clamp can shorten a read to less than the rest of the
    ; track, so advance within the track before moving to the next one.
    mov al, [kernel_chunk]
    add [kernel_sector], al
    cmp byte [kernel_sector], SECTORS_PER_TRACK + 1
    jb .kernel_read_loop

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

    ; --- VBE framebuffer discovery (INT 10h AX=4F00h/4F01h/4F02h) -----------
    ; Safe discovery: the BIOS is asked for its mode list, every mode is
    ; queried, and only a mode with a colour linear framebuffer that fits
    ; our single 4 MiB window is accepted. The 8x16 VGA font is copied out
    ; of the video BIOS ROM before the video mode is switched. If anything
    ; is missing the machine keeps the BIOS text mode, so the kernel falls
    ; back to the VGA text buffer.
    call vbe_discover

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

; ============================================================== VBE =========
; Framebuffer discovery with the VESA BIOS Extension (VBE).
;
; The structures and the mode selection rules follow Linux:
;   arch/x86/boot/vesa.h        vesa_general_info / vesa_mode_info
;   arch/x86/boot/video-vesa.c  vesa_probe() / vesa_set_mode()
;
; A mode is accepted when it is a supported graphics mode with a colour
; linear framebuffer (Linux's rule: attributes & 0x99 == 0x99 and a single
; plane), 24 or 32 bits per pixel, and a framebuffer that fits the single
; 4 MiB window the kernel maps. The horizontal resolution is derived from
; the pitch because some BIOSes leave it at zero.
;
; The framebuffer address comes from the mode info when the BIOS reports
; one; otherwise the VGA device's PCI memory BAR is read directly, the way
; Linux's fbdev picks up the framebuffer memory. The 8x16 VGA font is
; copied out of the video BIOS ROM. If anything is missing the machine
; keeps the BIOS text mode, no handover record is written, and the kernel
; falls back to the VGA text buffer.
;
; Low memory used by this block (DS is 0, so these are linear addresses):
;   0x6F00  scratch: the best mode, its geometry and the framebuffer base
;   0x6FC0  handover record: magic, base, pitch, w, h, bpp, masks, mode
;   0x6FFC  E820 entry count (written earlier)
;   0x7000  E820 entries (written earlier)
;   0x7400  VESA mode info buffer (256 B)
;   0x7500  VESA general info buffer (512 B)
;   0x9000  the 8x16 VGA font copied out of the video BIOS ROM
VBE_SCRATCH     equ 0x6F00
VBE_FB_RECORD   equ 0x6FC0
VBE_FONT_ADDR   equ 0x9000
VBE_MODE_ADDR   equ 0x7400
VBE_INFO_ADDR   equ 0x7500
VBE_FB_MAGIC    equ 0x31424648        ; 'HFB1'
VBE_MAX_WIDTH   equ 1024
VBE_MAX_HEIGHT  equ 768
VBE_MAX_FB_BYTES equ 0x400000         ; 4 MiB: one page-table window
VBE_MODE_LIMIT  equ 256

; vesa_general_info (Linux vesa.h)
VBE_OFF_SIGNATURE  equ 0x00            ; dword "VESA"
VBE_OFF_VERSION    equ 0x04            ; word
VBE_OFF_CAPABILITIES equ 0x0A          ; dword
VBE_OFF_VIDEO_MODE equ 0x0E            ; far_ptr (offset word, segment word)
VBE_OFF_TOTAL_MEM  equ 0x12            ; word, in 64 KiB blocks

; vesa_mode_info (Linux vesa.h)
VBE_OFF_MODE_ATTR  equ 0x00            ; word
VBE_OFF_PITCH      equ 0x10            ; word, logical_scan
VBE_OFF_HRES       equ 0x12            ; word
VBE_OFF_VRES       equ 0x14            ; word
VBE_OFF_PLANES     equ 0x18            ; byte
VBE_OFF_BPP        equ 0x19            ; byte
VBE_OFF_LAYOUT     equ 0x1B            ; byte, memory layout
VBE_OFF_LFB        equ 0x28            ; dword, linear framebuffer address

; scratch slots inside VBE_SCRATCH
VBE_SL_LIST_SEG  equ 0x00
VBE_SL_LIST_OFF  equ 0x02
VBE_SL_CUR_MODE  equ 0x04
VBE_SL_BEST_W    equ 0x08
VBE_SL_BEST_H    equ 0x0C
VBE_SL_BEST_BASE equ 0x10
VBE_SL_BEST_MODE equ 0x14
VBE_SL_BEST_PITCH equ 0x18
VBE_SL_BEST_BPP  equ 0x1C
VBE_SL_SCANNED   equ 0x20
VBE_SL_SET_AX    equ 0x24
VBE_SL_CUR_AX    equ 0x28
VBE_SL_CUR_PITCH equ 0x2C
VBE_SL_TARGET_BPP equ 0x30
VBE_SL_START_SEG equ 0x34
VBE_SL_START_OFF equ 0x36

vbe_discover:
    pusha

    ; --- controller info (vesa_general_info) ---
    ; Some BIOSes report an unsupported status in AX even though the video
    ; BIOS filled the block, so the block itself is validated instead.
    xor ax, ax
    mov es, ax
    mov di, VBE_INFO_ADDR
    mov ax, 0x4F00
    int 0x10
    cmp dword [VBE_INFO_ADDR + VBE_OFF_SIGNATURE], 0x41534556   ; "VESA"
    jne .no_vbe
    cmp word [VBE_INFO_ADDR + VBE_OFF_VERSION], 0x0102
    jb .no_vbe                           ; VBE 1.2 or newer is required

    ; --- locate the mode list ---
    ; The spec points at it with a far pointer (usually into the video
    ; BIOS ROM); some implementations copy the list into the info block
    ; instead, so scan the block as a fallback. With ES = 0 the offsets
    ; below are absolute addresses.
    mov ax, [VBE_INFO_ADDR + VBE_OFF_VIDEO_MODE + 2]  ; segment
    mov es, ax
    mov di, [VBE_INFO_ADDR + VBE_OFF_VIDEO_MODE]      ; offset
    call vbe_check_list
    jnc .list_ok
    xor ax, ax
    mov es, ax
    mov di, VBE_INFO_ADDR + 0x20                        ; probe the block
.probe:
    cmp di, VBE_INFO_ADDR + 0x40
    jae .no_modes
    call vbe_check_list
    jnc .list_ok
    add di, 2
    jmp .probe
.list_ok:
    mov [VBE_SCRATCH + VBE_SL_LIST_SEG], es
    mov [VBE_SCRATCH + VBE_SL_LIST_OFF], di
    ; remember where the list starts so it can be walked again
    mov [VBE_SCRATCH + VBE_SL_START_SEG], es
    mov [VBE_SCRATCH + VBE_SL_START_OFF], di

    mov word [VBE_SCRATCH + VBE_SL_BEST_W], 0
    mov word [VBE_SCRATCH + VBE_SL_BEST_H], 0
    mov dword [VBE_SCRATCH + VBE_SL_BEST_BASE], 0
    mov dword [VBE_SCRATCH + VBE_SL_BEST_MODE], 0
    mov dword [VBE_SCRATCH + VBE_SL_BEST_PITCH], 0
    mov dword [VBE_SCRATCH + VBE_SL_BEST_BPP], 0
    mov dword [VBE_SCRATCH + VBE_SL_SCANNED], 0

    ; --- walk the mode list ---
    ; 32 bits per pixel is preferred; 24 is the fallback.
    mov byte [VBE_SCRATCH + VBE_SL_TARGET_BPP], 32
    call vbe_scan_modes
    jnc .have_mode
    mov byte [VBE_SCRATCH + VBE_SL_TARGET_BPP], 24
    call vbe_scan_modes
    jc .no_mode
.have_mode:

    ; The framebuffer address: the mode info first, then the VGA device's
    ; PCI memory BAR (Linux's fbdev reads it from the PCI resources).
    mov eax, [VBE_SCRATCH + VBE_SL_BEST_BASE]
    test eax, eax
    jnz .have_base
    call vbe_find_pci_fb
    test eax, eax
    jz .no_mode
    mov [VBE_SCRATCH + VBE_SL_BEST_BASE], eax
.have_base:
    cmp eax, 0x00100000                ; a framebuffer lives above the ISA hole
    jb .no_mode
    ; the address is read as a dword, so it is always inside the 32-bit
    ; physical range the kernel can map

    ; the font is located by the kernel, which copies it out of the BIOS
    ; ROM area the way Linux's copy_font() does

    ; switch to the mode with the linear framebuffer enabled (Linux's
    ; vesa_set_mode(): the 0x4000 flag asks for the linear framebuffer)
    mov ax, 0x4F02
    mov bx, [VBE_SCRATCH + VBE_SL_BEST_MODE]
    or bx, 0x4000
    int 0x10
    mov [VBE_SCRATCH + VBE_SL_SET_AX], ax
    cmp ax, 0x004F
    je .mode_ok
    ; some BIOSes report an unsupported status even though the switch
    ; happened; VBE 4F03h reports the current mode in BX
    mov ax, 0x4F03
    int 0x10
    mov [VBE_SCRATCH + VBE_SL_CUR_AX], ax
    mov bx, [VBE_SCRATCH + VBE_SL_CUR_AX]
    and bx, 0x3FF
    cmp bx, [VBE_SCRATCH + VBE_SL_BEST_MODE]
    jne .no_set
.mode_ok:

    ; --- write the handover record ---
    mov dword [VBE_FB_RECORD + 0], VBE_FB_MAGIC
    mov eax, [VBE_SCRATCH + VBE_SL_BEST_BASE]
    mov [VBE_FB_RECORD + 4], eax        ; framebuffer physical address
    mov eax, [VBE_SCRATCH + VBE_SL_BEST_PITCH]
    mov [VBE_FB_RECORD + 8], eax        ; pitch
    movzx eax, word [VBE_SCRATCH + VBE_SL_BEST_W]
    mov [VBE_FB_RECORD + 12], eax       ; width
    movzx eax, word [VBE_SCRATCH + VBE_SL_BEST_H]
    mov [VBE_FB_RECORD + 16], eax       ; height
    mov eax, [VBE_SCRATCH + VBE_SL_BEST_BPP]
    mov [VBE_FB_RECORD + 20], eax       ; bits per pixel
    ; the standard direct-colour layout: 8 bits per channel
    mov word [VBE_FB_RECORD + 24], 0x1008   ; red: size 8, position 16
    mov word [VBE_FB_RECORD + 26], 0x0808   ; green: size 8, position 8
    mov word [VBE_FB_RECORD + 28], 0x0008   ; blue: size 8, position 0
    mov eax, [VBE_SCRATCH + VBE_SL_BEST_MODE]
    mov [VBE_FB_RECORD + 30], eax       ; VBE mode number
    ; framebuffer size = pitch * height
    mov eax, [VBE_SCRATCH + VBE_SL_BEST_H]
    imul eax, [VBE_SCRATCH + VBE_SL_BEST_PITCH]
    mov [VBE_FB_RECORD + 32], eax

    mov al, 'F'                        ; framebuffer mode is live
    call serial_putc
    mov al, 10
    call serial_putc
    popa
    ret

.no_vbe:
    mov al, 'a'                        ; no VESA BIOS extension
    jmp .report
.no_modes:
    mov al, 'b'                        ; no mode list
    jmp .report
.no_mode:
    mov al, 'c'                        ; no usable linear framebuffer mode
    jmp .report
.no_font:
    mov al, 'd'                        ; the VGA font copy failed
    jmp .report
.no_set:
    mov al, 'e'                        ; the mode switch was refused
.report:
    call serial_putc
    mov al, 10
    call serial_putc
    popa
    ret

; in: es:di = candidate mode list.
; out: CF set when the list looks valid (at least two mode numbers).
; ES:DI is preserved. The list itself is walked later and ends at the
; first entry that is not a mode number.
vbe_check_list:
    push ax
    push cx
    push di
    mov cx, 2
.check:
    mov ax, es:[di]
    cmp ax, 0x100
    jb .bad
    cmp ax, 0x7FF
    ja .bad
    add di, 2
    loop .check
    clc
    jmp .out
.bad:
    stc
.out:
    pop di
    pop cx
    pop ax
    ret

; Walk the mode list and pick the best mode with the target depth.
; in: the target bits per pixel in VBE_SL_TARGET_BPP.
; out: CF clear when a usable mode was found.
vbe_scan_modes:
    push ax
    push cx
    push dx
    push si
    push di
    ; restart at the beginning of the mode list
    mov ax, [VBE_SCRATCH + VBE_SL_START_SEG]
    mov [VBE_SCRATCH + VBE_SL_LIST_SEG], ax
    mov ax, [VBE_SCRATCH + VBE_SL_START_OFF]
    mov [VBE_SCRATCH + VBE_SL_LIST_OFF], ax
    mov dword [VBE_SCRATCH + VBE_SL_SCANNED], 0
.next_mode:
    cmp dword [VBE_SCRATCH + VBE_SL_SCANNED], VBE_MODE_LIMIT
    jae .list_done
    inc dword [VBE_SCRATCH + VBE_SL_SCANNED]
    mov es, [VBE_SCRATCH + VBE_SL_LIST_SEG]
    mov di, [VBE_SCRATCH + VBE_SL_LIST_OFF]
    mov cx, es:[di]                         ; the list lives in BIOS memory
    add di, 2
    mov [VBE_SCRATCH + VBE_SL_LIST_OFF], di
    cmp cx, 0x100
    jb .list_done                           ; not a mode number: end of list
    cmp cx, 0x7FF
    ja .list_done
    mov [VBE_SCRATCH + VBE_SL_CUR_MODE], cx

    ; query this mode (vesa_mode_info); clear the buffer first so a stale
    ; block from an earlier query cannot be mistaken for a valid one.
    ; CX must survive: it carries the mode number to the BIOS.
    xor ax, ax
    mov di, VBE_MODE_ADDR
    mov dx, 32
.clear_mode:
    mov word [di], ax
    add di, 2
    dec dx
    jnz .clear_mode
    xor ax, ax
    mov es, ax
    mov di, VBE_MODE_ADDR
    mov ax, 0x4F01
    int 0x10

    ; Linux's rule: supported, colour, graphics and linear framebuffer.
    mov ax, [VBE_MODE_ADDR + VBE_OFF_MODE_ATTR]
    and ax, 0x99
    cmp ax, 0x99
    jne .next
    cmp byte [VBE_MODE_ADDR + VBE_OFF_PLANES], 1
    jne .next
    mov dl, [VBE_SCRATCH + VBE_SL_TARGET_BPP]
    cmp byte [VBE_MODE_ADDR + VBE_OFF_BPP], dl
    jne .next

    ; The pitch and the height are the reliable fields; some BIOSes leave
    ; the horizontal resolution at zero, so the width is derived from the
    ; pitch and the depth.
    mov ax, [VBE_MODE_ADDR + VBE_OFF_PITCH]
    test ax, ax
    jz .next
    movzx eax, ax
    mov [VBE_SCRATCH + VBE_SL_CUR_PITCH], eax
    ; load the depth into ECX without touching EAX: AL holds the low byte
    ; of the pitch, so "mov al, [bpp]" would corrupt the dividend.
    movzx ecx, byte [VBE_MODE_ADDR + VBE_OFF_BPP]
    shr ecx, 3                     ; bytes per pixel
    test ecx, ecx
    jz .next
    xor edx, edx
    div ecx                        ; width = pitch / bytes per pixel
    mov cx, ax
    test cx, cx
    jz .next
    cmp cx, VBE_MAX_WIDTH
    ja .next
    mov dx, [VBE_MODE_ADDR + VBE_OFF_VRES]
    test dx, dx
    jz .next
    cmp dx, VBE_MAX_HEIGHT
    ja .next

    ; the framebuffer must fit the single 4 MiB window we map
    mov eax, [VBE_SCRATCH + VBE_SL_CUR_PITCH]
    imul eax, edx
    cmp eax, VBE_MAX_FB_BYTES
    ja .next

    ; keep the largest accepted geometry
    cmp cx, [VBE_SCRATCH + VBE_SL_BEST_W]
    jb .next
    je .same_width
    jmp .take
.same_width:
    cmp dx, [VBE_SCRATCH + VBE_SL_BEST_H]
    jbe .next
.take:
    mov [VBE_SCRATCH + VBE_SL_BEST_W], cx
    mov [VBE_SCRATCH + VBE_SL_BEST_H], dx
    mov eax, [VBE_SCRATCH + VBE_SL_CUR_MODE]
    mov [VBE_SCRATCH + VBE_SL_BEST_MODE], eax
    mov al, [VBE_MODE_ADDR + VBE_OFF_BPP]
    mov [VBE_SCRATCH + VBE_SL_BEST_BPP], al
    ; the pitch of the accepted mode
    mov eax, [VBE_SCRATCH + VBE_SL_CUR_PITCH]
    mov [VBE_SCRATCH + VBE_SL_BEST_PITCH], eax
    ; the framebuffer address, when the BIOS reports one
    mov eax, [VBE_MODE_ADDR + VBE_OFF_LFB]
    mov [VBE_SCRATCH + VBE_SL_BEST_BASE], eax
.next:
    jmp .next_mode
.list_done:
    cmp word [VBE_SCRATCH + VBE_SL_BEST_W], 0
    je .none
    clc
    jmp .out
.none:
    stc
.out:
    pop di
    pop si
    pop dx
    pop cx
    pop ax
    ret

; Read the framebuffer address from the VGA device's PCI memory BAR.
; Bus 0 is scanned for a display class device with a memory BAR, the way
; Linux's fbdev picks up the framebuffer memory. Returns the BAR base in
; EAX, or 0 when no such device is found.
vbe_find_pci_fb:
    push eax
    push ebx
    push ecx
    push edx
    xor ebx, ebx                    ; device number
.dev_loop:
    cmp ebx, 32
    jae .none
    ; configuration address 0x80000000 | (device << 11) | offset
    mov eax, 0x80000000
    mov ecx, ebx
    shl ecx, 11
    or eax, ecx
    mov edx, eax
    add edx, 0x00                  ; vendor and device id
    mov dx, 0xCF8
    out dx, eax
    mov dx, 0xCFC
    in eax, dx
    cmp al, 0xFF                   ; no device present
    je .next_dev
    ; class code: the base class is the top byte of the dword at 0x08
    mov eax, 0x80000000
    mov ecx, ebx
    shl ecx, 11
    or eax, ecx
    add eax, 0x08
    mov edx, eax
    mov dx, 0xCF8
    out dx, eax
    mov dx, 0xCFC
    in eax, dx
    shr eax, 24
    cmp al, 0x03                   ; display controller
    jne .next_dev
    ; memory BAR 0 at 0x10
    mov eax, 0x80000000
    mov ecx, ebx
    shl ecx, 11
    or eax, ecx
    add eax, 0x10
    mov edx, eax
    mov dx, 0xCF8
    out dx, eax
    mov dx, 0xCFC
    in eax, dx
    test al, 0x01                  ; must be a memory BAR
    jnz .next_dev
    and eax, 0xFFFFFFF0
    test eax, eax
    jz .next_dev
    jmp .found
.next_dev:
    inc ebx
    jmp .dev_loop
.found:
    pop edx
    pop ecx
    pop ebx
    pop eax
    ret
.none:
    xor eax, eax
    pop edx
    pop ecx
    pop ebx
    pop eax
    ret


; print AX as four hex digits (boot diagnostics)
vbe_print_hex:
    push ax
    push bx
    push cx
    mov bx, ax
    mov cx, 4
.next_digit:
    rol bx, 4
    mov al, bl
    and al, 0x0F
    add al, '0'
    cmp al, '9'
    jbe .emit
    add al, 7
.emit:
    call serial_putc
    loop .next_digit
    pop cx
    pop bx
    pop ax
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
