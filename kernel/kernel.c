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
#include "klog.h"
#include "paging.h"
#include "frame.h"
#include "pic.h"
#include "pit.h"
#include "gdt.h"
#include "usermode.h"

void kernel_main(void) {
    console_init();

    /* Boot banner, Linux-dmesg style. Markers stay inside the lines as
     * subsystem tags: qemu -serial file:serial.log is grepped for them. */
    klog_prefix();
    klog_write("Haku version 0.1 (i386) #1 HAKU-BOOT-OK\n");
    klog_prefix();
    klog_write("Command line: console=ttyS0 video=vga cpu=386+ mode=protected\n");

    idt_init();
    klog_prefix();
    klog_write("IDT-OK: 48 gates loaded (32 exceptions + 16 IRQs)\n");

    __asm int 3 /* #BP self-test: the handler prints, then we resume here */
    klog_prefix();
    klog_write("INT3-RESUMED: resumed after breakpoint self-test\n");

    /* Paging (Linux-style: traps are up before the MMU, IRQs still off
     * since stage2's cli — same ordering as Linux trap_init/long-mode
     * setup before paging_init). Identity-map the first 4 MiB, enable
     * CR0.PG, then prove the MMU is really translating. */
    paging_init();
    paging_enable();
    if (!paging_is_enabled() || paging_get_cr3() != PAGING_DIR_PHYS) {
        klog_prefix();
        klog_write("PAGING-FAIL: MMU did not enable, halting\n");
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
            klog_prefix();
            klog_write("PAGING-FAIL: R/W probe mismatch, halting\n");
            for (;;) {
                __asm hlt
            }
        }
    }
    klog_prefix();
    klog_write("PAGING-OK: 8MB identity map, cr3=0x20000, CR0.PG on\n");

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
        klog_prefix();
        klog_write("GDT-FAIL: TSS initialization failed, halting\n");
        for (;;) {
            __asm hlt
        }
    }
    klog_prefix();
    klog_write("GDT-OK: ring3 code/data loaded, TSS esp0 configured\n");

    if (!usermode_init()) {
        klog_prefix();
        klog_write("usermode setup failed, halting\n");
        for (;;) {
            __asm hlt
        }
    }

    /* PS/2 keyboard on IRQ1 (US layout, shift/caps handled in irq). */
    kbd_init();
    klog_prefix();
    klog_write("KBD-OK: PS/2 keyboard on IRQ1, US layout (unmasked)\n");

    /* Remap the 8259 (IRQs were colliding with CPU exceptions 8..15),
     * start the 100 Hz timer and open IRQ0 only. */
    pic_remap(0x20, 0x28);
    klog_prefix();
    klog_write("PIC-OK: 8259 remapped to 0x20/0x28\n");
    pit_init(100);
    klog_prefix();
    klog_write("PIT-OK: 100Hz timer on IRQ0\n");

    pic_clear_mask(0);
    cpu_sti();

    klog_prefix();
    klog_write("input: press two keys for the read() tests...\n");
    usermode_run();
    klog_prefix();
    klog_write("sched: returned unexpectedly, halting\n");
    for (;;) __asm hlt
}