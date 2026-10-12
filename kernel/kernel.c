/* Haku v0.1 — kernel entry point (32-bit protected mode, ring 0).
 *
 * The bootloader has loaded us at 0x10000 with flat segments and a valid
 * stack. From here: console -> IDT -> self-test -> halt (IRQs arrive in
 * later commits, once pic.c remaps the 8259).
 */
#include "cpu.h"
#include "console.h"
#include "heap.h"
#include "idt.h"
#include "kbd.h"
#include "paging.h"
#include "frame.h"
#include "pic.h"
#include "pit.h"
#include "gdt.h"
#include "usermode.h"

void kernel_main(void) {
    console_init();

    vga_write("============================================================\n");
    vga_write("  Haku v0.1 - hello from the C kernel (32-bit protected mode)\n");
    vga_write("============================================================\n");

    /* Marker for automated checks: qemu -serial file:serial.log */
    serial_write("HAKU-BOOT-OK cpu=386+ mode=protected video=vga serial=com1\n");

    idt_init();
    vga_write("IDT: 48 vectors + catch-all loaded\n");
    serial_write("IDT-OK gates=48\n");

    __asm int 3 /* #BP self-test: the handler prints, then we resume here */
    serial_write("INT3-RESUMED after breakpoint handler\n");

    /* Paging (Linux-style: traps are up before the MMU, IRQs still off
     * since stage2's cli — same ordering as Linux trap_init/long-mode
     * setup before paging_init). Identity-map the first 4 MiB, enable
     * CR0.PG, then prove the MMU is really translating. */
    paging_init();
    paging_enable();
    if (!paging_is_enabled() || paging_get_cr3() != PAGING_DIR_PHYS) {
        vga_write("PAGING FAILED to enable, halting\n");
        serial_write("PAGING-FAIL\n");
        for (;;) {
            __asm hlt
        }
    }
    {   /* R/W probe through the tables: low RAM, 3 MiB, 7 MiB. */
        volatile uint32_t *p1 = (volatile uint32_t *)0x2000u;
        volatile uint32_t *p2 = (volatile uint32_t *)0x300000u;
        volatile uint32_t *p3 = (volatile uint32_t *)0x700000u;
        *p1 = 0x12345678u;
        *p2 = 0xA5A5A5A5u;
        *p3 = 0x5A5A5A5Au;
        if (*p1 != 0x12345678u || *p2 != 0xA5A5A5A5u || *p3 != 0x5A5A5A5Au) {
            vga_write("PAGING probe mismatch, halting\n");
            serial_write("PAGING-FAIL probe\n");
            for (;;) {
                __asm hlt
            }
        }
    }
    vga_write("PAGING: 8 MiB identity mapped, CR0.PG on\n");
    serial_write("PAGING-OK idmap=8MB cr3=0x20000\n");

    /* Heap: free-list allocator over the identity-mapped 1–4 MiB range. */
    heap_init();
    if (!heap_self_test()) {
        for (;;) {
            __asm hlt
        }
    }

    /* E820-backed frame inventory. Allocation is limited to the identity-
     * mapped 8 MiB until temporary mappings support higher physical frames. */
    frame_init();
    if (!frame_self_test()) {
        for (;;) {
            __asm hlt
        }
    }

    if (!gdt_init()) {
        vga_write("GDT/TSS initialization failed, halting\n");
        serial_write("GDT-FAIL\n");
        for (;;) {
            __asm hlt
        }
    }
    serial_write("GDT-OK ring3 tss esp0 configured\n");

    if (!usermode_init()) {
        vga_write("User address space setup failed, halting\n");
        for (;;) {
            __asm hlt
        }
    }

    /* PS/2 keyboard on IRQ1 (US layout, shift/caps handled in irq). */
    kbd_init();
    serial_write("KBD-OK irq1 irq1=unmasked\n");
    vga_write("PS/2 keyboard ready (IRQ1). Type in QEMU window.\n");

    /* Remap the 8259 (IRQs were colliding with CPU exceptions 8..15),
     * start the 100 Hz timer and open IRQ0 only. */
    pic_remap(0x20, 0x28);
    serial_write("PIC-OK remap=20/28\n");
    pit_init(100);
    serial_write("PIT-OK 100Hz\n");

    pic_clear_mask(0);
    cpu_sti();

    vga_write("ELF32 processes loaded; press two keys for read() tests...\n");
    usermode_run();
    vga_write("Scheduler returned unexpectedly; halting.\n");
    for (;;) __asm hlt
}