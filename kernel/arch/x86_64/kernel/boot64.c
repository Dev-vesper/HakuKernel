/* Haku x86-64 bring-up: the first 64-bit C code the kernel runs.
 *
 * entry64.asm enters long mode, installs an identity map of the first
 * 1 GiB with 2 MiB pages, and calls kernel64_main with a 16-byte aligned
 * stack. Reaching this function at all proves the pieces a port depends on:
 * the long-mode transition, the 64-bit code segment, the Microsoft x64
 * calling convention across the asm/C boundary, and that the linker and
 * pe2bin handle a PE32+ image.
 *
 * Everything else (heap, frame allocator, interrupts, syscalls, userland)
 * is still the 32-bit kernel; see ROADMAP.md for the porting order.
 */

void serial_write64(const char *text);
void cpu_halt64(void);

void kernel64_main(void) {
    /* No klog yet: it is a 32-bit module with its own locks and port
     * writes, so the stub talks to COM1 through the entry asm. */
    serial_write64("HAKU64-BOOT-OK: long mode, 1 GiB identity map, 64-bit C\r\n");
    serial_write64("HAKU64-READY: 32-bit kernel still boots the system\r\n");
    for (;;) {
        cpu_halt64();
    }
}
