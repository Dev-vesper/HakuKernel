#ifndef HAKU_ATA_H
#define HAKU_ATA_H

#include "cpu.h"

int ata_init(void);
int ata_read_sectors(uint32_t lba, uint8_t count, void *buffer);
int ata_write_sectors(uint32_t lba, uint8_t count, const void *buffer);
uint32_t ata_sector_count(void);
uint32_t ata_device_count(void);
int ata_select_device(uint32_t index);
/* Sector-cache counters since ata_init (the self-test reports the first
 * two accesses, the running totals are for diagnostics). */
void ata_cache_stats(uint32_t *hits, uint32_t *misses);

#endif
