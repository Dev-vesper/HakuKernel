#ifndef HAKU_FAT12_H
#define HAKU_FAT12_H

#include "cpu.h"

typedef struct {
    uint8_t short_name[11];
    uint16_t first_cluster;
    uint32_t size;
    uint32_t position;
    uint8_t flags;
    int opened;
} fat12_file_t;

#define FAT12_OPEN_READ   0x01u
#define FAT12_OPEN_WRITE  0x02u
#define FAT12_OPEN_CREATE 0x04u
#define FAT12_OPEN_TRUNC  0x08u
#define FAT12_OPEN_APPEND 0x10u
#define FAT12_OPEN_EXCL   0x20u

#define FAT12_ERR_IO        1
#define FAT12_ERR_NOT_FOUND 2
#define FAT12_ERR_EXISTS    3
#define FAT12_ERR_NO_SPACE  4
#define FAT12_ERR_INVALID   5

int fat12_mount(void);
int fat12_read_file(const char *name, void *buffer, uint32_t capacity,
                    uint32_t *file_size);
int fat12_file_open(const char *name, uint8_t flags, fat12_file_t *file);
int fat12_file_close(fat12_file_t *file);
int fat12_file_read(fat12_file_t *file, void *buffer, uint32_t count,
                    uint32_t *read_count);
int fat12_file_write(fat12_file_t *file, const void *buffer, uint32_t count,
                     uint32_t *write_count);
int fat12_file_seek(fat12_file_t *file, int32_t offset, uint32_t whence,
                    uint32_t *new_position);

#endif
