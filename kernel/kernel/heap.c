/* Haku heap implementation.
 *
 * Layout: [hdr1][payload1][hdr2][payload2]...[hdrN][payloadN]
 * All headers are linked in address order (sorted by address), so
 * coalescing is O(1) when freeing — just check prev/next neighbours.
 *
 * The first block spans [HEAP_START, HEAP_END) and starts as FREE.
 */
#include "heap.h"
#include "console.h"
#include "klog.h"
#include "sync.h"

static heap_block_t *heap_head = 0;
static spinlock_t heap_lock = SPINLOCK_INIT;

/* Align size up to 16 bytes (MSVC stack alignment + future SIMD). */
static size_t align16(size_t n) {
    return (n + 15u) & ~(size_t)15u;
}

/* Validate a pointer belongs to the heap and its magic is sane. */
static int valid_ptr(const void *p) {
    return p != 0 &&
           (uint32_t)p >= HEAP_START + sizeof(heap_block_t) &&
           (uint32_t)p < HEAP_END;
}

void heap_init(void) {
    heap_block_t *first = (heap_block_t *)HEAP_START;
    first->prev = 0;
    first->next = 0;
    first->size = HEAP_END - HEAP_START - sizeof(heap_block_t);
    first->magic = HEAP_MAGIC_FREE;
    heap_head = first;
}

size_t heap_free_bytes(void) {
    size_t total = 0;
    irqflags_t flags = spin_lock_irqsave(&heap_lock);
    for (heap_block_t *b = heap_head; b; b = b->next)
        if (b->magic == HEAP_MAGIC_FREE)
            total += b->size;
    spin_unlock_irqrestore(&heap_lock, flags);
    return total;
}

/* Split a free block if the leftover is large enough to be useful. */
static void try_split(heap_block_t *b, size_t need) {
    size_t overhead = sizeof(heap_block_t);
    if (b->size >= need + overhead + HEAP_MIN_PAYLOAD) {
        heap_block_t *rest = (heap_block_t *)((uint8_t *)(b + 1) + need);
        rest->prev = b;
        rest->next = b->next;
        rest->size = b->size - need - overhead;
        rest->magic = HEAP_MAGIC_FREE;
        if (b->next) b->next->prev = rest;
        b->next = rest;
        b->size = need;
    }
}

static void *kmalloc_unlocked(size_t size) {
    if (size == 0) return 0;
    size = align16(size);
    for (heap_block_t *b = heap_head; b; b = b->next) {
        if (b->magic == HEAP_MAGIC_FREE && b->size >= size) {
            try_split(b, size);
            b->magic = HEAP_MAGIC_USED;
            return (void *)(b + 1);
        }
    }
    return 0; /* OOM */
}

void *kmalloc(size_t size) {
    void *result;
    irqflags_t flags = spin_lock_irqsave(&heap_lock);
    result = kmalloc_unlocked(size);
    spin_unlock_irqrestore(&heap_lock, flags);
    return result;
}

void *kzalloc(size_t size) {
    void *p = kmalloc(size);
    if (p) memset(p, 0, size);
    return p;
}

/* Coalesce with next if it's free (and vice versa for prev). */
static void coalesce(heap_block_t *b) {
    /* Merge with next. */
    if (b->next && b->next->magic == HEAP_MAGIC_FREE) {
        b->size += sizeof(heap_block_t) + b->next->size;
        b->next = b->next->next;
        if (b->next) b->next->prev = b;
    }
    /* Merge with prev. */
    if (b->prev && b->prev->magic == HEAP_MAGIC_FREE) {
        b->prev->size += sizeof(heap_block_t) + b->size;
        b->prev->next = b->next;
        if (b->next) b->next->prev = b->prev;
    }
}

static void kfree_unlocked(void *ptr) {
    if (!valid_ptr(ptr)) return;
    heap_block_t *b = (heap_block_t *)ptr - 1;
    if (b->magic != HEAP_MAGIC_USED) return; /* double-free or corruption */
    b->magic = HEAP_MAGIC_FREE;
    coalesce(b);
}

void kfree(void *ptr) {
    irqflags_t flags = spin_lock_irqsave(&heap_lock);
    kfree_unlocked(ptr);
    spin_unlock_irqrestore(&heap_lock, flags);
}

static void *krealloc_unlocked(void *ptr, size_t new_size) {
    if (!ptr) return kmalloc_unlocked(new_size);
    if (new_size == 0) { kfree_unlocked(ptr); return 0; }
    if (!valid_ptr(ptr)) return 0;

    heap_block_t *b = (heap_block_t *)ptr - 1;
    if (b->magic != HEAP_MAGIC_USED) return 0;

    size_t aligned = align16(new_size);
    if (b->size >= aligned) {
        /* Shrink in place; split the tail if useful. */
        try_split(b, aligned);
        return ptr;
    }

    /* Try to grow into the next free block. */
    if (b->next && b->next->magic == HEAP_MAGIC_FREE &&
        b->size + sizeof(heap_block_t) + b->next->size >= aligned) {
        b->size += sizeof(heap_block_t) + b->next->size;
        b->next = b->next->next;
        if (b->next) b->next->prev = b;
        try_split(b, aligned);
        return ptr;
    }

    /* Can't grow in place — allocate, copy, free. */
    void *fresh = kmalloc_unlocked(new_size);
    if (!fresh) return 0;
    memcpy(fresh, ptr, b->size < new_size ? b->size : new_size);
    kfree_unlocked(ptr);
    return fresh;
}

void *krealloc(void *ptr, size_t new_size) {
    void *result;
    irqflags_t flags = spin_lock_irqsave(&heap_lock);
    result = krealloc_unlocked(ptr, new_size);
    spin_unlock_irqrestore(&heap_lock, flags);
    return result;
}

/* ------------------------------------------------------------------ */
/* Self-test: exercises alloc, free, coalesce, realloc, and edge cases. */
/* ------------------------------------------------------------------ */
int heap_self_test(void) {
    int pass = 1;

    /* 1. Basic alloc + write + free. */
    char *a = (char *)kmalloc(64);
    if (!a) { pass = 0; goto done; }
    memset(a, 'A', 64);
    if (a[0] != 'A' || a[63] != 'A') pass = 0;

    /* 2. Second alloc gets a different block. */
    char *b = (char *)kmalloc(128);
    if (!b || b == a) { pass = 0; goto done; }
    memset(b, 'B', 128);

    /* 3. Free a, alloc smaller — should reuse or split. */
    kfree(a);
    char *c = (char *)kmalloc(32);
    if (!c) { pass = 0; goto done; }

    /* 4. Coalesce: free b and c, then alloc a large block. */
    kfree(b);
    kfree(c);
    char *d = (char *)kmalloc(1024);
    if (!d) { pass = 0; goto done; }

    /* 5. Zeroed alloc. */
    void *e = kzalloc(256);
    if (!e) { pass = 0; goto done; }
    for (int i = 0; i < 256; i++)
        if (((uint8_t *)e)[i] != 0) { pass = 0; break; }

    /* 6. Realloc: grow. */
    void *f = krealloc(e, 512);
    if (!f) { pass = 0; goto done; }

    /* 7. Realloc: shrink. */
    f = krealloc(f, 128);
    if (!f) { pass = 0; goto done; }

    /* 8. kfree(NULL) is safe. */
    kfree(0);
    kfree(d);
    kfree(f);

    /* 9. OOM: request more than the whole heap. */
    void *g = kmalloc(HEAP_END); /* way too big */
    if (g != 0) { pass = 0; kfree(g); }

done:
    if (pass) {
        klog_prefix();
        klog_write("HEAP-OK: kmalloc self-test PASS, bytes=");
        klog_dec(heap_free_bytes());
        klog_write("\n");
    } else {
        klog_prefix();
        klog_write("HEAP-FAIL: self-test FAIL\n");
    }
    return pass;
}
