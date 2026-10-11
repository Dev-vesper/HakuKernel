; =============================================================================
; MyOS v0.1 — interrupt stubs (NASM -> COFF object for link.exe)
;
; Each CPU exception gets a tiny stub that pushes a uniform frame:
;   [dummy error code unless the CPU pushed a real one] + vector number,
; then jumps to isr_common. IRQs (vectors 32..47) go to irq_common instead.
; isr_common saves everything, calls the C dispatcher with a regs_t pointer,
; restores state and irets. All IDT gates are interrupt gates, so IF is
; already clear on entry — hence the leading cli is just belt and braces.
; =============================================================================

[bits 32]

global _idt_load, _cpu_sti, _cpu_cli
extern _isr_handler, _irq_handler

; NOTE: win32 output needs an EXPLICIT section for global labels to bind.
; Without this, NASM emits the code but drops every definition from the
; symbol table (verified by dumping the .obj: sect=0 on all globals).
section .text

_idt_load:                       ; void idt_load(uint32_t idtr_addr)
    mov eax, [esp+4]
    lidt [eax]
    ret

_cpu_sti:
    sti
    ret

_cpu_cli:
    cli
    ret

; ---- CPU exceptions: dummy error code except where the CPU pushes one -----
; Error-code vectors (Intel SDM): 8, 10, 11, 12, 13, 14, 17.
%macro ISR_NOERR 1
global _isr%1
_isr%1:
    cli
    push 0
    push %1
    jmp isr_common
%endmacro

%macro ISR_ERR 1
global _isr%1
_isr%1:
    cli
    push %1
    jmp isr_common
%endmacro

ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_NOERR 21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_NOERR 30
ISR_NOERR 31

; ---- hardware IRQs: vectors 32..47 -----------------------------------------
%macro IRQ 2
global _irq%1
_irq%1:
    cli
    push 0
    push %2
    jmp irq_common
%endmacro

%assign i 0
%rep 16
IRQ i, i+32
%assign i i+1
%endrep

; ---- catch-all for vectors 48..255 ------------------------------------------
global _isr_default
_isr_default:
    cli
    push 0
    push 255
    jmp isr_common

; ---- common entry ------------------------------------------------------------
isr_common:
    pusha                         ; eax,ecx,edx,ebx,esp,ebp,esi,edi
    mov ax, ds
    push eax                      ; ds
    mov ax, 0x10                  ; kernel data segment
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    push esp                      ; regs_t* argument
    call _isr_handler
    add esp, 4                    ; drop argument
    pop eax
    mov ds, ax                    ; restore ds (es/fs/gs never changed)
    popa
    add esp, 8                    ; drop vector number + error code
    iret

irq_common:
    pusha
    mov ax, ds
    push eax
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    push esp
    call _irq_handler
    add esp, 4
    pop eax
    mov ds, ax
    popa
    add esp, 8
    iret
