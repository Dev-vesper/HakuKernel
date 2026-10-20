/* Haku frame allocator: E820-backed physical-page inventory.
 *
 * The bitmap tracks all usable RAM below 128 MiB and every tracked frame
 * is reachable through the supervisor direct map, so allocation is not
 * limited to the first 8 MiB identity window.
 */
#include "frame.h"
#include "heap.h"
#include "klog.h"
#include "panic.h"
#include "paging.h"
#include "sync.h"

static spinlock_t frame_lock = SPINLOCK_INIT;

#define E820_MAP_PHYS 0x7000u
#define E820_MAX_ENTRIES 24u
#define KERNEL_LOAD_START 0x00010000u
#define KERNEL_LOAD_END   0x00080000u
#define KERNEL_HEAP_START 0x00100000u
#define KERNEL_HEAP_END   0x00400000u
#define BOOT_STACK_START  0x00080000u
#define BOOT_STACK_END    0x00090000u
#define ISA_HOLE_START    0x000A0000u
#define ISA_HOLE_END      0x00100000u

#define FRAME_PHYS_LIMIT (FRAME_ENTRIES * FRAME_SIZE)
#define FRAME_HIGH_WATERMARK (8u * 1024u * 1024u)

typedef struct {
    uint32_t base_low;
    uint32_t base_high;
    uint32_t length_low;
    uint32_t length_high;
    uint32_t type;
    uint32_t attributes;
} e820_entry_t;

static uint32_t *frame_bitmap;
static uint32_t *frame_usable;

static void bitmap_set(uint32_t *bitmap, uint32_t frame) {
    bitmap[frame / 32u] |= 1u << (frame % 32u);
}

static void bitmap_clear(uint32_t *bitmap, uint32_t frame) {
    bitmap[frame / 32u] &= ~(1u << (frame % 32u));
}

static int bitmap_test(const uint32_t *bitmap, uint32_t frame) {
    return (bitmap[frame / 32u] >> (frame % 32u)) & 1u;
}

static uint32_t e820_count(void) {
    return *(volatile uint32_t *)0x6FFCu;
}

/* Convert a 64-bit E820 range to the part intersecting our tracked RAM. */
static int e820_clipped_range(const e820_entry_t *entry,
                              uint32_t *start, uint32_t *end) {
    uint32_t range_end;

    if (!(entry->attributes & 1u)) return 0;
    if ((entry->length_low | entry->length_high) == 0 ||
        entry->base_high != 0 ||
        entry->base_low >= FRAME_PHYS_LIMIT)
        return 0;

    *start = entry->base_low;
    if (entry->length_high != 0 ||
        entry->length_low > FRAME_PHYS_LIMIT - entry->base_low) {
        range_end = FRAME_PHYS_LIMIT;
    } else {
        range_end = entry->base_low + entry->length_low;
    }
    if (range_end <= *start) return 0;
    *end = range_end;
    return 1;
}

static void set_usable_range(uint32_t start, uint32_t end) {
    uint32_t first = (start + FRAME_SIZE - 1u) / FRAME_SIZE;
    uint32_t last = end / FRAME_SIZE;
    if (last > FRAME_ENTRIES) last = FRAME_ENTRIES;
    for (uint32_t frame = first; frame < last; frame++) {
        bitmap_set(frame_usable, frame);
        bitmap_clear(frame_bitmap, frame);
    }
}

static void reserve_range(uint32_t start, uint32_t end) {
    uint32_t first = start / FRAME_SIZE;
    uint32_t last = (end + FRAME_SIZE - 1u) / FRAME_SIZE;
    if (last > FRAME_ENTRIES) last = FRAME_ENTRIES;
    for (uint32_t frame = first; frame < last; frame++) {
        bitmap_clear(frame_usable, frame);
        bitmap_set(frame_bitmap, frame);
    }
}

static void reserve_e820_non_ram(uint32_t count) {
    volatile e820_entry_t *entries = (volatile e820_entry_t *)E820_MAP_PHYS;
    for (uint32_t i = 0; i < count; i++) {
        e820_entry_t entry;
        uint32_t start, end;
        entry.base_low = entries[i].base_low;
        entry.base_high = entries[i].base_high;
        entry.length_low = entries[i].length_low;
        entry.length_high = entries[i].length_high;
        entry.type = entries[i].type;
        entry.attributes = entries[i].attributes;
        if (entry.type != E820_RAM &&
            e820_clipped_range(&entry, &start, &end)) {
            reserve_range(start, end);
        }
    }
}

static void reserve_kernel_regions(void) {
    reserve_range(0, ISA_HOLE_END); /* low firmware, loader data and ISA hole */
    reserve_range(KERNEL_LOAD_START, KERNEL_LOAD_END);
    reserve_range(PAGING_DIR_PHYS, PAGING_TABLE1_PHYS + 0x1000u);
    reserve_range(BOOT_STACK_START, BOOT_STACK_END);
    reserve_range(KERNEL_HEAP_START, KERNEL_HEAP_END);
}

/* Keep a device range (such as a framebuffer aperture) out of the pool.
 * Ranges outside the tracked window are ignored, and a range that is
 * already reserved stays reserved. */
void frame_reserve_range(uint32_t start, uint32_t end) {
    irqflags_t flags;
    if (!frame_bitmap || !frame_usable || end <= start) return;
    flags = spin_lock_irqsave(&frame_lock);
    reserve_range(start, end);
    spin_unlock_irqrestore(&frame_lock, flags);
}

void frame_init(void) {
    uint32_t count = e820_count();
    volatile e820_entry_t *entries = (volatile e820_entry_t *)E820_MAP_PHYS;

    if (count == 0 || count > E820_MAX_ENTRIES) {
        panic("E820-FAIL: map missing or invalid");
    }

    frame_bitmap = (uint32_t *)kmalloc(FRAME_BMAP_BYTES);
    frame_usable = (uint32_t *)kmalloc(FRAME_BMAP_BYTES);
    if (!frame_bitmap || !frame_usable) {
        panic("FRAME-FAIL: no heap for bitmaps");
    }
    memset(frame_bitmap, 0xFF, FRAME_BMAP_BYTES);
    memset(frame_usable, 0, FRAME_BMAP_BYTES);

    /* Start with usable E820 RAM, then let any overlapping reserved range
     * win, independent of the order BIOS returned its entries. */
    for (uint32_t i = 0; i < count; i++) {
        e820_entry_t entry;
        uint32_t start, end;
        entry.base_low = entries[i].base_low;
        entry.base_high = entries[i].base_high;
        entry.length_low = entries[i].length_low;
        entry.length_high = entries[i].length_high;
        entry.type = entries[i].type;
        entry.attributes = entries[i].attributes;
        if (entry.type == E820_RAM &&
            e820_clipped_range(&entry, &start, &end)) {
            set_usable_range(start, end);
        }
    }
    reserve_e820_non_ram(count);
    reserve_kernel_regions();

    /* Linux-style RAM map dump, then the inventory summary. */
    klog_prefix();
    klog_write("BIOS-provided physical RAM map:\n");
    for (uint32_t i = 0; i < count; i++) {
        uint32_t base_lo = entries[i].base_low;
        uint32_t base_hi = entries[i].base_high;
        uint32_t end_lo = base_lo + entries[i].length_low;
        uint32_t end_hi = base_hi + entries[i].length_high +
                         (end_lo < base_lo ? 1u : 0u);
        const char *kind;
        end_lo -= 1u;
        if (end_lo == 0xFFFFFFFFu) end_hi -= 1u;
        switch (entries[i].type) {
        case 1u: kind = "usable"; break;
        case 2u: kind = "reserved"; break;
        case 3u: kind = "ACPI reclaimable"; break;
        case 4u: kind = "ACPI NVS"; break;
        case 5u: kind = "bad"; break;
        default: kind = "unknown"; break;
        }
        klog_prefix();
        klog_write("BIOS-e820: [mem 0x");
        klog_hex_raw(base_hi);
        klog_hex_raw(base_lo);
        klog_write("-0x");
        klog_hex_raw(end_hi);
        klog_hex_raw(end_lo);
        klog_write("] ");
        klog_write(kind);
        klog_write("\n");
    }
    klog_prefix();
    klog_write("E820-OK: ");
    klog_dec(count);
    klog_write(" map entries\n");
}

void *frame_alloc(void) {
    void *result = 0;
    irqflags_t flags;
    if (!frame_bitmap || !frame_usable) return 0;
    flags = spin_lock_irqsave(&frame_lock);
    for (uint32_t frame = 0; frame < FRAME_ENTRIES; frame++) {
        if (bitmap_test(frame_usable, frame) &&
            !bitmap_test(frame_bitmap, frame)) {
            bitmap_set(frame_bitmap, frame);
            result = (void *)(frame * FRAME_SIZE);
            break;
        }
    }
    spin_unlock_irqrestore(&frame_lock, flags);
    return result;
}

void frame_free(void *phys) {
    uint32_t address = (uint32_t)phys;
    uint32_t frame;
    irqflags_t flags;
    if (!frame_bitmap || !frame_usable ||
        (address & (FRAME_SIZE - 1u)) != 0 ||
        address >= FRAME_PHYS_LIMIT)
        return;
    frame = address / FRAME_SIZE;
    flags = spin_lock_irqsave(&frame_lock);
    if (bitmap_test(frame_usable, frame) &&
        bitmap_test(frame_bitmap, frame))
        bitmap_clear(frame_bitmap, frame);
    spin_unlock_irqrestore(&frame_lock, flags);
}

void *frame_alloc_pages(uint32_t n) {
    uint32_t run = 0;
    uint32_t start = 0;
    void *result = 0;
    irqflags_t flags;
    if (!frame_bitmap || !frame_usable || n == 0 ||
        n > FRAME_ENTRIES)
        return 0;

    flags = spin_lock_irqsave(&frame_lock);
    for (uint32_t frame = 0; frame < FRAME_ENTRIES; frame++) {
        if (bitmap_test(frame_usable, frame) &&
            !bitmap_test(frame_bitmap, frame)) {
            if (run == 0) start = frame;
            run++;
            if (run == n) {
                for (uint32_t allocated = start;
                     allocated < start + n; allocated++)
                    bitmap_set(frame_bitmap, allocated);
                result = (void *)(start * FRAME_SIZE);
                break;
            }
        } else {
            run = 0;
        }
    }
    spin_unlock_irqrestore(&frame_lock, flags);
    return result;
}

void frame_free_pages(void *phys, uint32_t n) {
    uint32_t address = (uint32_t)phys;
    uint32_t start;
    irqflags_t flags;
    if (!frame_bitmap || !frame_usable || n == 0 ||
        (address & (FRAME_SIZE - 1u)) != 0 ||
        address >= FRAME_PHYS_LIMIT)
        return;
    start = address / FRAME_SIZE;
    if (n > FRAME_ENTRIES - start) return;

    flags = spin_lock_irqsave(&frame_lock);
    for (uint32_t frame = start; frame < start + n; frame++) {
        if (!bitmap_test(frame_usable, frame) ||
            !bitmap_test(frame_bitmap, frame)) {
            spin_unlock_irqrestore(&frame_lock, flags);
            return;
        }
    }
    for (uint32_t frame = start; frame < start + n; frame++)
        bitmap_clear(frame_bitmap, frame);
    spin_unlock_irqrestore(&frame_lock, flags);
}

uint32_t frame_total_count(void) { return FRAME_ENTRIES; }

uint32_t frame_usable_count(void) {
    uint32_t count = 0;
    irqflags_t flags;
    if (!frame_usable) return 0;
    flags = spin_lock_irqsave(&frame_lock);
    for (uint32_t frame = 0; frame < FRAME_ENTRIES; frame++)
        if (bitmap_test(frame_usable, frame)) count++;
    spin_unlock_irqrestore(&frame_lock, flags);
    return count;
}

uint32_t frame_free_count(void) {
    uint32_t count = 0;
    irqflags_t flags;
    if (!frame_bitmap || !frame_usable) return 0;
    flags = spin_lock_irqsave(&frame_lock);
    for (uint32_t frame = 0; frame < FRAME_ENTRIES; frame++)
        if (bitmap_test(frame_usable, frame) &&
            !bitmap_test(frame_bitmap, frame))
            count++;
    spin_unlock_irqrestore(&frame_lock, flags);
    return count;
}

int frame_self_test(void) {
    int pass = 1;
    uint32_t high_frame = 0;
    uint32_t initial_free = frame_free_count();
    if (frame_usable_count() == 0 || initial_free == 0) {
        pass = 0;
        goto done;
    }
    void *a = frame_alloc();
    void *b = frame_alloc();
    if (!a || !b || a == b ||
        (uint32_t)a >= FRAME_PHYS_LIMIT ||
        (uint32_t)b >= FRAME_PHYS_LIMIT) {
        pass = 0;
        goto done;
    }
    *(volatile uint32_t *)PHYS_TO_VIRT(a) = 0x11111111u;
    *(volatile uint32_t *)PHYS_TO_VIRT(b) = 0x22222222u;
    if (*(volatile uint32_t *)PHYS_TO_VIRT(a) != 0x11111111u) pass = 0;

    frame_free(a);
    frame_free(b);
    if (frame_free_count() != initial_free) pass = 0;

    /* Reserved and misaligned addresses must never become allocatable. */
    frame_free((void *)0);
    frame_free((void *)KERNEL_HEAP_START);
    frame_free((void *)(KERNEL_HEAP_START + 1u));
    if (frame_free_count() != initial_free) pass = 0;

    {
        void *c = frame_alloc();
        if (!c) {
            pass = 0;
            goto done;
        }
        {
            uint32_t value = *(volatile uint32_t *)PHYS_TO_VIRT(c);
            if (value != 0x11111111u && value != 0x22222222u) pass = 0;
        }
        frame_free(c);
    }

    /* Walk the free list until a frame above the old 8 MiB identity
     * window appears, proving high physical memory is allocatable and
     * reachable through the direct map. The link lives in each frame, so
     * the test stays O(1) in stack space. */
    {
        void *chain = 0;
        void *high = 0;
        uint32_t allocated = 0;
        while (allocated < FRAME_ENTRIES) {
            void *page = frame_alloc();
            uint32_t link;
            if (!page) break;
            link = (uint32_t)chain;
            *(volatile uint32_t *)PHYS_TO_VIRT(page) = link;
            chain = page;
            allocated++;
            if ((uint32_t)page >= FRAME_HIGH_WATERMARK) {
                high = page;
                high_frame = (uint32_t)page;
                *(volatile uint32_t *)PHYS_TO_VIRT(page) = 0x33333333u;
                if (*(volatile uint32_t *)PHYS_TO_VIRT(page) != 0x33333333u)
                    pass = 0;
                *(volatile uint32_t *)PHYS_TO_VIRT(page) = link;
                break;
            }
        }
        if (!high) pass = 0;
        while (chain) {
            void *next = (void *)*(volatile uint32_t *)PHYS_TO_VIRT(chain);
            frame_free(chain);
            chain = next;
        }
        if (frame_free_count() != initial_free) pass = 0;
    }

    {
        void *pages = frame_alloc_pages(4);
        if (!pages || (uint32_t)pages >= FRAME_PHYS_LIMIT) {
            pass = 0;
            goto done;
        }
        frame_free_pages(pages, 4);
        if (frame_free_count() != initial_free) pass = 0;
    }

done:
    if (pass) {
        klog_prefix();
        klog_write("FRAME-OK: E820 self-test PASS, free=");
        klog_dec(frame_free_count());
        klog_write(" usable=");
        klog_dec(frame_usable_count());
        klog_write(" alloc-limit=128MB high=");
        klog_hex(high_frame);
        klog_write("\n");
    } else {
        klog_prefix();
        klog_write("FRAME-FAIL: self-test FAIL\n");
    }
    return pass;
}
