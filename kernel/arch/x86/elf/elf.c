#include "elf.h"
#include "frame.h"
#include "usermode.h"

#define ELFCLASS32 1u
#define ELFDATA2LSB 1u
#define EM_386 3u
#define ET_EXEC 2u
#define PT_LOAD 1u
#define PT_DYNAMIC 2u
#define PT_INTERP 3u
#define PF_X 1u
#define PF_W 2u
#define PF_R 4u
#define ELF_MAX_PHDRS 32u
#define USER_IMAGE_MIN 0x40000000u
#define USER_STACK_BOTTOM (USER_STACK_TOP - 2u * USER_PAGE_SIZE)

#pragma pack(push, 1)
typedef struct {
    uint8_t ident[16];
    uint16_t type;
    uint16_t machine;
    uint32_t version;
    uint32_t entry;
    uint32_t phoff;
    uint32_t shoff;
    uint32_t flags;
    uint16_t ehsize;
    uint16_t phentsize;
    uint16_t phnum;
    uint16_t shentsize;
    uint16_t shnum;
    uint16_t shstrndx;
} elf32_header_t;

typedef struct {
    uint32_t type;
    uint32_t offset;
    uint32_t vaddr;
    uint32_t paddr;
    uint32_t filesz;
    uint32_t memsz;
    uint32_t flags;
    uint32_t align;
} elf32_phdr_t;
#pragma pack(pop)

static int in_image(uint32_t offset, uint32_t length, uint32_t image_size) {
    return offset <= image_size && length <= image_size - offset;
}

static int segment_bounds(const elf32_phdr_t *phdr, uint32_t *page_start,
                          uint32_t *page_end) {
    uint32_t end;
    if (phdr->memsz == 0 || phdr->filesz > phdr->memsz ||
        phdr->vaddr < USER_IMAGE_MIN ||
        phdr->memsz > 0xFFFFFFFFu - phdr->vaddr)
        return 0;
    end = phdr->vaddr + phdr->memsz;
    if (end > USER_STACK_BOTTOM || end <= phdr->vaddr) return 0;
    *page_start = phdr->vaddr & 0xFFFFF000u;
    if (end > 0xFFFFF000u) return 0;
    *page_end = (end + 0xFFFu) & 0xFFFFF000u;
    if (*page_end <= *page_start) return 0;
    return 1;
}

static int valid_segment(const elf32_phdr_t *phdr, uint32_t image_size,
                         uint32_t *page_start, uint32_t *page_end) {
    if (phdr->type != PT_LOAD) return 1;
    if (!(phdr->flags & PF_R) ||
        ((phdr->flags & (PF_W | PF_X)) == (PF_W | PF_X)) ||
        !segment_bounds(phdr, page_start, page_end) ||
        !in_image(phdr->offset, phdr->filesz, image_size))
        return 0;
    if (phdr->align > 1u &&
        ((phdr->align & (phdr->align - 1u)) != 0 ||
         (phdr->vaddr & (phdr->align - 1u)) !=
         (phdr->offset & (phdr->align - 1u))))
        return 0;
    return 1;
}

static int page_is_mapped(const address_space_t *space, uint32_t address,
                          uint32_t *physical) {
    uint32_t *directory = (uint32_t *)space->directory_phys;
    uint32_t pde = directory[address >> 22];
    uint32_t table_phys;
    uint32_t pte;
    if (!(pde & PAGE_PRESENT)) return 0;
    table_phys = pde & 0xFFFFF000u;
    if (table_phys >= PAGING_IDMAP_MB * 1024u * 1024u) return 0;
    pte = ((uint32_t *)table_phys)[(address >> 12) & 0x3FFu];
    if (!(pte & PAGE_PRESENT)) return 0;
    *physical = pte & 0xFFFFF000u;
    return 1;
}

static int map_zeroed_pages(address_space_t *space, uint32_t start,
                            uint32_t end, int writable) {
    for (uint32_t address = start; address < end; address += USER_PAGE_SIZE) {
        void *page = frame_alloc();
        if (!page) return 0;
        memset(page, 0, USER_PAGE_SIZE);
        if (!paging_map_user_page(space, address, (uint32_t)page, writable)) {
            frame_free(page);
            return 0;
        }
    }
    return 1;
}

static int copy_segment(const uint8_t *image, const elf32_phdr_t *phdr,
                        const address_space_t *space) {
    uint32_t copied = 0;
    while (copied < phdr->filesz) {
        uint32_t address = phdr->vaddr + copied;
        uint32_t physical;
        uint32_t page_offset = address & 0xFFFu;
        uint32_t amount = USER_PAGE_SIZE - page_offset;
        if (amount > phdr->filesz - copied)
            amount = phdr->filesz - copied;
        if (!page_is_mapped(space, address, &physical)) return 0;
        memcpy((void *)(physical + page_offset), image + phdr->offset + copied,
               amount);
        copied += amount;
    }
    return 1;
}

int elf_load_executable(const uint8_t *image, uint32_t image_size,
                        loaded_elf_t *loaded) {
    elf32_header_t header;
    elf32_phdr_t phdrs[ELF_MAX_PHDRS];
    uint32_t starts[ELF_MAX_PHDRS];
    uint32_t ends[ELF_MAX_PHDRS];
    uint32_t load_count = 0;
    int entry_is_executable = 0;

    if (!image || !loaded || image_size < sizeof(header)) return 0;
    memcpy(&header, image, sizeof(header));
    if (header.ident[0] != 0x7Fu || header.ident[1] != 'E' ||
        header.ident[2] != 'L' || header.ident[3] != 'F' ||
        header.ident[4] != ELFCLASS32 || header.ident[5] != ELFDATA2LSB ||
        header.ident[6] != 1u || header.type != ET_EXEC ||
        header.machine != EM_386 || header.version != 1u ||
        header.ehsize != sizeof(header) ||
        header.phentsize != sizeof(elf32_phdr_t) ||
        header.phnum == 0 || header.phnum > ELF_MAX_PHDRS ||
        !in_image(header.phoff,
                  (uint32_t)header.phnum * sizeof(elf32_phdr_t), image_size))
        return 0;

    memset(loaded, 0, sizeof(*loaded));
    for (uint32_t i = 0; i < header.phnum; i++) {
        elf32_phdr_t phdr;
        memcpy(&phdr, image + header.phoff + i * sizeof(phdr), sizeof(phdr));
        if (phdr.type == PT_INTERP || phdr.type == PT_DYNAMIC) return 0;
        if (phdr.type != PT_LOAD) continue;
        if (!valid_segment(&phdr, image_size, &starts[load_count],
                           &ends[load_count]))
            return 0;
        for (uint32_t j = 0; j < load_count; j++)
            if (starts[load_count] < ends[j] && starts[j] < ends[load_count])
                return 0;
        if ((phdr.flags & PF_X) && header.entry >= phdr.vaddr &&
            header.entry - phdr.vaddr < phdr.memsz)
            entry_is_executable = 1;
        phdrs[load_count++] = phdr;
    }
    if (load_count == 0 || !entry_is_executable ||
        (header.entry & 0xFFF00000u) == 0xFFF00000u)
        return 0;

    if (!paging_create_user_space(&loaded->address_space)) return 0;
    for (uint32_t i = 0; i < load_count; i++) {
        int writable = (phdrs[i].flags & PF_W) != 0;
        if (!map_zeroed_pages(&loaded->address_space, starts[i], ends[i],
                              writable) ||
            !copy_segment(image, &phdrs[i], &loaded->address_space))
            goto fail;
    }
    if (!map_zeroed_pages(&loaded->address_space, USER_STACK_BOTTOM,
                          USER_STACK_TOP, 1))
        goto fail;

    loaded->entry = header.entry;
    loaded->user_stack = USER_STACK_TOP;
    return 1;

fail:
    paging_destroy_user_space(&loaded->address_space);
    return 0;
}
