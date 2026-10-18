#include "ata.h"
#include "console.h"
#include "klog.h"
#include "sync.h"

#define ATA_DATA       0x1F0u
#define ATA_ERROR      0x1F1u
#define ATA_SECTOR_CNT 0x1F2u
#define ATA_LBA_LOW    0x1F3u
#define ATA_LBA_MID    0x1F4u
#define ATA_LBA_HIGH   0x1F5u
#define ATA_DRIVE      0x1F6u
#define ATA_STATUS     0x1F7u
#define ATA_COMMAND    0x1F7u
#define ATA_CONTROL    0x3F6u

#define ATA_SR_ERR  0x01u
#define ATA_SR_DRQ  0x08u
#define ATA_SR_DF   0x20u
#define ATA_SR_BSY  0x80u
#define ATA_TIMEOUT 1000000u

static uint32_t primary_master_sectors;
static uint16_t identify_words[256];
/* Serializes command sequences and the shared register/data window. A
 * task-context spinlock: the polling transfer may be preempted, and any
 * other ATA caller busy-waits until the current sequence completes. */
static spinlock_t ata_lock = SPINLOCK_INIT;

static int wait_status(uint8_t required) {
    for (uint32_t attempt = 0; attempt < ATA_TIMEOUT; attempt++) {
        uint8_t status = inb(ATA_STATUS);
        if (status == 0 || status == 0xFFu) return 0;
        if (status & ATA_SR_BSY) continue;
        if (status & (ATA_SR_ERR | ATA_SR_DF)) return 0;
        if ((status & required) == required) return 1;
    }
    return 0;
}

static int ata_init_unlocked(void) {
    uint8_t status;
    primary_master_sectors = 0;

    outb(ATA_CONTROL, 0x02u); /* polling PIO: disable device IRQs */
    outb(ATA_DRIVE, 0xA0u);   /* primary master, CHS-compatible selection */
    io_wait();
    inb(ATA_STATUS);
    inb(ATA_STATUS);
    inb(ATA_STATUS);
    inb(ATA_STATUS);
    outb(ATA_SECTOR_CNT, 0);
    outb(ATA_LBA_LOW, 0);
    outb(ATA_LBA_MID, 0);
    outb(ATA_LBA_HIGH, 0);
    outb(ATA_COMMAND, 0xECu); /* IDENTIFY DEVICE */

    status = inb(ATA_STATUS);
    if (status == 0 || status == 0xFFu ||
        !wait_status(ATA_SR_DRQ)) {
        klog_prefix();
        klog_write("ATA-FAIL: identify primary master\n");
        return 0;
    }

    for (uint32_t i = 0; i < 256; i++)
        identify_words[i] = inw(ATA_DATA);

    if (!(identify_words[49] & (1u << 9))) {
        klog_prefix();
        klog_write("ATA-FAIL: LBA unsupported\n");
        return 0;
    }
    primary_master_sectors = (uint32_t)identify_words[60] |
                             ((uint32_t)identify_words[61] << 16);
    if (!primary_master_sectors) {
        klog_prefix();
        klog_write("ATA-FAIL: empty LBA capacity\n");
        return 0;
    }

    klog_prefix();
    klog_write("ata: ATA-OK primary-master LBA28 sectors=");
    klog_dec(primary_master_sectors);
    klog_write("\n");
    return 1;
}

int ata_init(void) {
    int result;
    spin_lock(&ata_lock);
    result = ata_init_unlocked();
    spin_unlock(&ata_lock);
    return result;
}

static int ata_read_sectors_unlocked(uint32_t lba, uint8_t count, void *buffer) {
    uint8_t *destination = (uint8_t *)buffer;
    if (!buffer || !count || !primary_master_sectors ||
        lba >= primary_master_sectors ||
        (uint32_t)count > primary_master_sectors - lba ||
        lba >= 0x10000000u ||
        (uint32_t)count > 0x10000000u - lba)
        return 0;

    for (uint32_t sector = 0; sector < count; sector++) {
        uint32_t current_lba = lba + sector;
        outb(ATA_DRIVE, (uint8_t)(0xE0u | ((current_lba >> 24) & 0x0Fu)));
        io_wait();
        outb(ATA_SECTOR_CNT, 1);
        outb(ATA_LBA_LOW, (uint8_t)current_lba);
        outb(ATA_LBA_MID, (uint8_t)(current_lba >> 8));
        outb(ATA_LBA_HIGH, (uint8_t)(current_lba >> 16));
        outb(ATA_COMMAND, 0x20u); /* READ SECTORS, one sector */
        if (!wait_status(ATA_SR_DRQ)) {
            klog_prefix();
            klog_write("ATA-READ-FAIL: lba=");
            klog_dec(current_lba);
            klog_write(" error=");
            klog_hex(inb(ATA_ERROR));
            klog_write("\n");
            return 0;
        }

        for (uint32_t word = 0; word < 256; word++) {
            uint16_t value = inw(ATA_DATA);
            destination[sector * 512u + word * 2u] = (uint8_t)value;
            destination[sector * 512u + word * 2u + 1u] =
                (uint8_t)(value >> 8);
        }
        if (!wait_status(0)) {
            klog_prefix();
            klog_write("ATA-READ-FAIL: completion lba=");
            klog_dec(current_lba);
            klog_write("\n");
            return 0;
        }
    }
    return 1;
}

int ata_read_sectors(uint32_t lba, uint8_t count, void *buffer) {
    int result;
    spin_lock(&ata_lock);
    result = ata_read_sectors_unlocked(lba, count, buffer);
    spin_unlock(&ata_lock);
    return result;
}

static int ata_write_sectors_unlocked(uint32_t lba, uint8_t count,
                                      const void *buffer) {
    const uint8_t *source = (const uint8_t *)buffer;
    if (!buffer || !count || !primary_master_sectors ||
        lba >= primary_master_sectors ||
        (uint32_t)count > primary_master_sectors - lba ||
        lba >= 0x10000000u ||
        (uint32_t)count > 0x10000000u - lba)
        return 0;

    for (uint32_t sector = 0; sector < count; sector++) {
        uint32_t current_lba = lba + sector;
        outb(ATA_DRIVE, (uint8_t)(0xE0u | ((current_lba >> 24) & 0x0Fu)));
        io_wait();
        outb(ATA_SECTOR_CNT, 1);
        outb(ATA_LBA_LOW, (uint8_t)current_lba);
        outb(ATA_LBA_MID, (uint8_t)(current_lba >> 8));
        outb(ATA_LBA_HIGH, (uint8_t)(current_lba >> 16));
        outb(ATA_COMMAND, 0x30u); /* WRITE SECTORS, one sector */
        if (!wait_status(ATA_SR_DRQ)) {
            klog_prefix();
            klog_write("ATA-WRITE-FAIL: lba=");
            klog_dec(current_lba);
            klog_write(" error=");
            klog_hex(inb(ATA_ERROR));
            klog_write("\n");
            return 0;
        }

        for (uint32_t word = 0; word < 256; word++) {
            uint32_t offset = sector * 512u + word * 2u;
            uint16_t value = (uint16_t)source[offset] |
                             ((uint16_t)source[offset + 1u] << 8);
            outw(ATA_DATA, value);
        }
        if (!wait_status(0)) {
            klog_prefix();
            klog_write("ATA-WRITE-FAIL: completion lba=");
            klog_dec(current_lba);
            klog_write("\n");
            return 0;
        }
    }
    return 1;
}

int ata_write_sectors(uint32_t lba, uint8_t count, const void *buffer) {
    int result;
    spin_lock(&ata_lock);
    result = ata_write_sectors_unlocked(lba, count, buffer);
    spin_unlock(&ata_lock);
    return result;
}

uint32_t ata_sector_count(void) {
    /* A 32-bit aligned load; the capacity is written once by ata_init. */
    return primary_master_sectors;
}
