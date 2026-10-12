/* MyOS v0.1 — kernel entry point (32-bit protected mode, ring 0).
 *
 * The bootloader has loaded us at 0x10000 with flat segments and a valid
 * stack. From here: console -> IDT -> self-test -> halt (IRQs arrive in
 * later commits, once pic.c remaps the 8259).
 */
#include "cpu.h"
#include "console.h"
#include "idt.h"
#include "pic.h"
#include "pit.h"

void kernel_main(void) {
    console_init();

    vga_write("============================================================\n");
    vga_write("  MyOS v0.1 - hello from the C kernel (32-bit protected mode)\n");
    vga_write("============================================================\n");

    /* Marker for automated checks: qemu -serial file:serial.log */
    serial_write("MYOS-BOOT-OK cpu=386+ mode=protected video=vga serial=com1\n");

    idt_init();
    vga_write("IDT: 48 vectors + catch-all loaded\n");
    serial_write("IDT-OK gates=48\n");

    __asm int 3 /* #BP self-test: the handler prints, then we resume here */
    serial_write("INT3-RESUMED after breakpoint handler\n");

    /* Remap the 8259 (IRQs were colliding with CPU exceptions 8..15),
     * start the 100 Hz timer and open IRQ0 only. */
    pic_remap(0x20, 0x28);
    serial_write("PIC-OK remap=20/28\n");
    pit_init(100);
    serial_write("PIT-OK 100Hz\n");
    pic_clear_mask(0);
    cpu_sti();

    vga_write("Timer running (see the corner clock). Idling on hlt...\n");
    for (;;) {
        __asm hlt
    }
}
