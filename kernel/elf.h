/* Minimal ELF32 loader for static i386 ET_EXEC images. */
#ifndef HAKU_ELF_H
#define HAKU_ELF_H

#include "paging.h"

typedef struct {
    address_space_t address_space;
    uint32_t entry;
    uint32_t user_stack;
} loaded_elf_t;

int elf_load_executable(const uint8_t *image, uint32_t image_size,
                        loaded_elf_t *loaded);

#endif
