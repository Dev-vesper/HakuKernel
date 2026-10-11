/* =============================================================================
 * MyOS v0.1 — kernel in C (freestanding, 32-bit, no CRT, no headers)
 *
 * Build: cl.exe /c (COFF object) + entry.obj --link.exe--> kernel.pe.
 * Rules for this file so the image links cleanly:
 *   - No #include (works without vcvarsall INCLUDE paths).
 *   - No float / no 64-bit ints (would pull in CRT helpers).
 *   - memset/memcpy/memmove/memcmp are defined here, so any compiler-
 *     generated call to them resolves inside the link (no CRT needed).
 * Serial init mirrors Linux early_serial_init (16550: DLAB/divisor/8N1).
 * ========================================================================== */

typedef unsigned char  uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int   uint32_t;
typedef unsigned int   size_t;

/* ------------------------------------------------- forward decls ---- */
/* kernel_main() is defined first for readability; link.exe /ENTRY:_entry
   (via entry.obj) decides the actual image layout. */
void kernel_main(void);
static void vga_clear(uint8_t color);
static void vga_write(const char *s);
static void serial_init(void);
static void serial_write(const char *s);

/* ============================================================ entry ====== */
void kernel_main(void) {
    vga_clear(0x1F); /* white on blue, classic */
    serial_init();

    vga_write("============================================================\n");
    vga_write("  MyOS v0.1 - hello from the C kernel (32-bit protected mode)\n");
    vga_write("============================================================\n");
    vga_write("Bootloader: OK | A20: OK | GDT: OK | VGA: OK | Serial: OK\n");
    vga_write("\nSystem halted. Press the QEMU close button (or power off).\n");

    /* Marker for automated checks: qemu -serial file:serial.log */
    serial_write("MYOS-BOOT-OK cpu=386+ mode=protected video=vga serial=com1\n");

    for (;;) {
        __asm hlt
    }
}

/* ---------------------------------------------------------- tiny libc ---- */
void *memset(void *dst, int c, size_t n) {
    uint8_t *p = (uint8_t *)dst;
    while (n--) *p++ = (uint8_t)c;
    return dst;
}

void *memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    if (d < s) {
        while (n--) *d++ = *s++;
    } else if (d > s) {
        d += n; s += n;
        while (n--) *--d = *--s;
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *p = (const uint8_t *)a, *q = (const uint8_t *)b;
    while (n--) {
        if (*p != *q) return *p - *q;
        p++; q++;
    }
    return 0;
}

size_t strlen(const char *s) {
    size_t n = 0;
    while (*s++) n++;
    return n;
}

/* --------------------------------------------------------------- ports ---- */
static void outb(uint16_t port, uint8_t val) {
    __asm {
        mov dx, port
        mov al, val
        out dx, al
    }
}

static uint8_t inb(uint16_t port) {
    uint8_t value;
    __asm {
        mov dx, port
        in al, dx
        mov value, al
    }
    return value;
}

/* ----------------------------------------------------------- VGA text ---- */
#define VGA_COLS 80
#define VGA_ROWS 25

static volatile uint16_t *const VGA = (uint16_t *)0xB8000;
static uint8_t vga_row = 0;
static uint8_t vga_col = 0;
static uint8_t vga_color = 0x0F; /* bright white on black */

static void vga_clear(uint8_t color) {
    uint16_t blank = (uint16_t)((color << 8) | ' ');
    for (int i = 0; i < VGA_COLS * VGA_ROWS; i++)
        VGA[i] = blank;
    vga_row = 0;
    vga_col = 0;
    vga_color = color;
}

static void vga_scroll(void) {
    memmove((void *)VGA, (void *)(VGA + VGA_COLS),
            (VGA_ROWS - 1) * VGA_COLS * sizeof(uint16_t));
    uint16_t blank = (uint16_t)((vga_color << 8) | ' ');
    for (int x = 0; x < VGA_COLS; x++)
        VGA[(VGA_ROWS - 1) * VGA_COLS + x] = blank;
    vga_row = VGA_ROWS - 1;
}

static void vga_putc(char c) {
    if (c == '\n') {
        vga_col = 0;
        if (++vga_row >= VGA_ROWS) vga_scroll();
        return;
    }
    VGA[vga_row * VGA_COLS + vga_col] =
        (uint16_t)((vga_color << 8) | (uint8_t)c);
    if (++vga_col >= VGA_COLS) {
        vga_col = 0;
        if (++vga_row >= VGA_ROWS) vga_scroll();
    }
}

static void vga_write(const char *s) {
    while (*s) vga_putc(*s++);
}

/* ------------------------------------------------------------ serial ---- */
#define COM1 0x3F8

static int serial_ready(void) {
    return inb(COM1 + 5) & 0x20;
}

static void serial_init(void) {
    outb(COM1 + 1, 0x00); /* disable interrupts */
    outb(COM1 + 3, 0x80); /* enable divisor latch */
    outb(COM1 + 0, 0x03); /* 38400 baud (divisor 3) */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03); /* 8 bits, no parity, one stop */
    outb(COM1 + 2, 0xC7); /* FIFO on, 14-byte threshold */
    outb(COM1 + 4, 0x0B); /* RTS+DTR, loopback test bit off */
}

static void serial_putc(char c) {
    while (!serial_ready()) { /* spin */ }
    outb(COM1, (uint8_t)c);
}

static void serial_write(const char *s) {
    while (*s) {
        if (*s == '\n') serial_putc('\r');
        serial_putc(*s++);
    }
}
