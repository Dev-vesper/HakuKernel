#include <haku/errno.h>
#include <haku/stdlib.h>
#include <haku/string.h>
#include <haku/unistd.h>

#define BLOCK_MAGIC 0x48414B55u
#define GROW_SIZE 16384u
#define MIN_SPLIT 8u

typedef struct haku_block {
    size_t size;
    struct haku_block *next;
    struct haku_block *prev;
    int free;
    unsigned int magic;
    unsigned int reserved;
} haku_block_t;

static haku_block_t *block_list;
static haku_block_t *block_tail;

static size_t align_size(size_t size) {
    return (size + 7u) & ~7u;
}

static void split_block(haku_block_t *block, size_t wanted) {
    haku_block_t *rest;
    if (block->size < wanted + sizeof(haku_block_t) + MIN_SPLIT) return;
    rest = (haku_block_t *)((char *)(block + 1) + wanted);
    rest->size = block->size - wanted - sizeof(haku_block_t);
    rest->next = block->next;
    rest->prev = block;
    rest->free = 1;
    rest->magic = BLOCK_MAGIC;
    rest->reserved = 0;
    if (rest->next)
        rest->next->prev = rest;
    else
        block_tail = rest;
    block->next = rest;
    block->size = wanted;
}

static haku_block_t *grow_heap(size_t wanted) {
    size_t chunk = wanted + sizeof(haku_block_t);
    haku_block_t *block;
    if (chunk < GROW_SIZE) chunk = GROW_SIZE;
    chunk = (chunk + 0xFFFu) & ~0xFFFu;
    block = (haku_block_t *)sbrk((int)chunk);
    if (block == (void *)-1) return NULL;
    if (block_tail && block_tail->free &&
        (char *)(block_tail + 1) + block_tail->size == (char *)block) {
        block_tail->size += chunk;
        return block_tail;
    }
    block->size = chunk - sizeof(haku_block_t);
    block->next = NULL;
    block->prev = block_tail;
    block->free = 1;
    block->magic = BLOCK_MAGIC;
    block->reserved = 0;
    if (block_tail)
        block_tail->next = block;
    else
        block_list = block;
    block_tail = block;
    return block;
}

void *malloc(size_t size) {
    haku_block_t *block;
    size_t wanted;

    if (size > 0xFFFF0000u) {
        errno = ENOMEM;
        return NULL;
    }
    wanted = align_size(size ? size : 1u);

    for (block = block_list; block; block = block->next) {
        if (block->free && block->size >= wanted) {
            split_block(block, wanted);
            block->free = 0;
            return block + 1;
        }
    }
    block = grow_heap(wanted);
    if (!block) {
        errno = ENOMEM;
        return NULL;
    }
    split_block(block, wanted);
    block->free = 0;
    return block + 1;
}

void free(void *pointer) {
    haku_block_t *block;
    if (!pointer) return;
    block = ((haku_block_t *)pointer) - 1;
    if (block->magic != BLOCK_MAGIC || block->free) return;
    block->free = 1;
    if (block->next && block->next->free) {
        block->size += sizeof(haku_block_t) + block->next->size;
        block->next = block->next->next;
        if (block->next)
            block->next->prev = block;
        else
            block_tail = block;
    }
    if (block->prev && block->prev->free) {
        haku_block_t *previous = block->prev;
        previous->size += sizeof(haku_block_t) + block->size;
        previous->next = block->next;
        if (previous->next)
            previous->next->prev = previous;
        else
            block_tail = previous;
    }
}

void *calloc(size_t count, size_t size) {
    void *pointer;
    size_t total;
    if (count && size > 0xFFFFFFFFu / count) {
        errno = ENOMEM;
        return NULL;
    }
    total = count * size;
    pointer = malloc(total);
    if (pointer) memset(pointer, 0, total);
    return pointer;
}

void *realloc(void *pointer, size_t size) {
    haku_block_t *block;
    void *fresh;
    size_t copy;
    if (!pointer) return malloc(size);
    if (size == 0) {
        free(pointer);
        return NULL;
    }
    block = ((haku_block_t *)pointer) - 1;
    if (block->magic != BLOCK_MAGIC) return NULL;
    if (block->size >= align_size(size)) {
        split_block(block, align_size(size));
        return pointer;
    }
    fresh = malloc(size);
    if (!fresh) return NULL;
    copy = block->size < size ? block->size : size;
    memcpy(fresh, pointer, copy);
    free(pointer);
    return fresh;
}

int atoi(const char *text) {
    int sign = 1;
    int value = 0;
    while (*text == ' ' || *text == '\t' || *text == '\n' ||
           *text == '\r' || *text == '\f' || *text == '\v')
        text++;
    if (*text == '-') {
        sign = -1;
        text++;
    } else if (*text == '+') {
        text++;
    }
    while (*text >= '0' && *text <= '9') {
        value = value * 10 + (*text - '0');
        text++;
    }
    return value * sign;
}
