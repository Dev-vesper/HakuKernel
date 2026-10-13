#include "fat12.h"
#include "ata.h"
#include "console.h"

#define FAT12_SECTOR_SIZE 512u
#define FAT12_MAX_SECTORS_PER_CLUSTER 8u
#define FAT12_ATTR_DIRECTORY 0x10u
#define FAT12_ATTR_VOLUME 0x08u
#define FAT12_ATTR_LONG_NAME 0x0Fu
#define FAT12_CLUSTER_BAD 0xFF7u
#define FAT12_CLUSTER_EOC 0xFF8u
#define FAT12_CLUSTER_FREE 0x000u
#define FAT12_CLUSTER_END 0xFFFu
#define FAT12_ATTR_ARCHIVE 0x20u
#define FAT12_SEEK_SET 0u
#define FAT12_SEEK_CUR 1u
#define FAT12_SEEK_END 2u

typedef struct {
    uint32_t total_sectors;
    uint32_t fat_start;
    uint32_t fat_sectors;
    uint32_t root_start;
    uint32_t root_sectors;
    uint32_t data_start;
    uint32_t cluster_count;
    uint8_t sectors_per_cluster;
    uint8_t fat_count;
    uint16_t root_entries;
    int mounted;
} fat12_volume_t;

static fat12_volume_t volume;
static uint8_t sector_buffer[FAT12_SECTOR_SIZE];

static uint16_t read_u16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t read_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int read_volume_sector(uint32_t relative_lba, void *buffer) {
    if (relative_lba >= volume.total_sectors) return 0;
    return ata_read_sectors(relative_lba, 1, buffer);
}

const char *fat12_error_name(int result) {
    if (result == 1) return "OK";
    switch (-result) {
    case FAT12_ERR_IO: return "IO";
    case FAT12_ERR_NOT_FOUND: return "NOT_FOUND";
    case FAT12_ERR_EXISTS: return "EXISTS";
    case FAT12_ERR_NO_SPACE: return "NO_SPACE";
    case FAT12_ERR_INVALID: return "INVALID";
    default: return "UNKNOWN";
    }
}

int fat12_test_sector_info(uint32_t *fat_start, uint32_t *fat_sectors,
                           uint32_t *fat_count, uint32_t *cluster_count,
                           uint32_t *sectors_per_cluster) {
    if (!fat_start || !fat_sectors || !fat_count || !cluster_count ||
        !sectors_per_cluster)
        return -FAT12_ERR_INVALID;
    if (!volume.mounted) return -FAT12_ERR_IO;
    *fat_start = volume.fat_start;
    *fat_sectors = volume.fat_sectors;
    *fat_count = volume.fat_count;
    *cluster_count = volume.cluster_count;
    *sectors_per_cluster = volume.sectors_per_cluster;
    return 1;
}

int fat12_test_read_sector(uint32_t lba, void *buffer) {
    if (!volume.mounted || !buffer) return -FAT12_ERR_INVALID;
    if (lba >= volume.total_sectors) return -FAT12_ERR_INVALID;
    return ata_read_sectors(lba, 1, buffer) ? 1 : -FAT12_ERR_IO;
}

int fat12_test_write_sector(uint32_t lba, const void *buffer) {
    if (!volume.mounted || !buffer) return -FAT12_ERR_INVALID;
    if (lba >= volume.total_sectors) return -FAT12_ERR_INVALID;
    return ata_write_sectors(lba, 1, buffer) ? 1 : -FAT12_ERR_IO;
}

int fat12_mount(void) {
    uint32_t root_bytes;
    uint32_t data_sectors;
    uint32_t fat_bytes;
    uint32_t required_fat_bytes;
    uint16_t total16;

    memset(&volume, 0, sizeof(volume));
    if (!ata_init()) return -FAT12_ERR_IO;
    if (!ata_read_sectors(0, 1, sector_buffer)) {
        serial_write("FAT12-FAIL boot sector read\n");
        return -FAT12_ERR_IO;
    }
    total16 = read_u16(&sector_buffer[19]);
    volume.total_sectors = total16 ? total16 : read_u32(&sector_buffer[32]);
    volume.sectors_per_cluster = sector_buffer[13];
    volume.fat_count = sector_buffer[16];
    volume.root_entries = read_u16(&sector_buffer[17]);
    volume.fat_sectors = read_u16(&sector_buffer[22]);
    volume.fat_start = read_u16(&sector_buffer[14]);

    if (read_u16(&sector_buffer[11]) != FAT12_SECTOR_SIZE ||
        !volume.sectors_per_cluster ||
        volume.sectors_per_cluster > FAT12_MAX_SECTORS_PER_CLUSTER ||
        (volume.sectors_per_cluster & (volume.sectors_per_cluster - 1u)) ||
        read_u16(&sector_buffer[14]) != 1u ||
        !volume.fat_count || volume.fat_count > 2u ||
        !volume.root_entries || !volume.fat_sectors ||
        !volume.total_sectors ||
        volume.total_sectors > ata_sector_count() ||
        sector_buffer[510] != 0x55u || sector_buffer[511] != 0xAAu) {
        serial_write("FAT12-FAIL invalid BPB\n");
        return -FAT12_ERR_INVALID;
    }

    root_bytes = (uint32_t)volume.root_entries * 32u;
    volume.root_sectors = (root_bytes + FAT12_SECTOR_SIZE - 1u) /
                          FAT12_SECTOR_SIZE;
    volume.root_start = volume.fat_start +
                        (uint32_t)volume.fat_count * volume.fat_sectors;
    volume.data_start = volume.root_start + volume.root_sectors;
    if (volume.data_start >= volume.total_sectors) {
        serial_write("FAT12-FAIL invalid volume layout\n");
        return -FAT12_ERR_INVALID;
    }

    data_sectors = volume.total_sectors - volume.data_start;
    volume.cluster_count = data_sectors / volume.sectors_per_cluster;
    fat_bytes = volume.fat_sectors * FAT12_SECTOR_SIZE;
    required_fat_bytes = ((volume.cluster_count + 2u) * 3u + 1u) / 2u;
    if (volume.cluster_count >= 4085u ||
        required_fat_bytes > fat_bytes) {
        serial_write("FAT12-FAIL cluster table bounds\n");
        return -FAT12_ERR_INVALID;
    }

    volume.mounted = 1;
    serial_write("FAT12-MOUNT-OK sectors=");
    serial_dec(volume.total_sectors);
    serial_write(" clusters=");
    serial_dec(volume.cluster_count);
    serial_write("\n");
    return 1;
}

static int make_short_name(const char *name, uint8_t result[11]) {
    uint32_t base_len = 0;
    uint32_t ext_len = 0;
    int in_extension = 0;

    memset(result, ' ', 11);
    if (!name || !*name) return 0;
    for (uint32_t i = 0; name[i]; i++) {
        char ch = name[i];
        uint32_t position;
        if (ch == '.') {
            if (in_extension || base_len == 0) return 0;
            in_extension = 1;
            continue;
        }
        if (ch <= 0x20 || ch >= 0x7Fu || ch == '/' || ch == '\\' ||
            ch == ':' || ch == '*' || ch == '?' || ch == '"' ||
            ch == '<' || ch == '>' || ch == '|' || ch == '+' ||
            ch == ',' || ch == ';' || ch == '=' || ch == '[' ||
            ch == ']')
            return 0;
        if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
        if (in_extension) {
            if (ext_len >= 3) return 0;
            position = 8u + ext_len++;
        } else {
            if (base_len >= 8) return 0;
            position = base_len++;
        }
        result[position] = (uint8_t)ch;
    }
    return base_len != 0;
}

static int find_root_entry(const uint8_t short_name[11], uint8_t result[32],
                           uint32_t *sector_lba, uint32_t *sector_offset) {
    uint32_t entries_read = 0;
    for (uint32_t sector = 0; sector < volume.root_sectors; sector++) {
        if (!read_volume_sector(volume.root_start + sector, sector_buffer))
            return -1;
        for (uint32_t offset = 0; offset < FAT12_SECTOR_SIZE;
             offset += 32u) {
            const uint8_t *entry = &sector_buffer[offset];
            if (entries_read++ >= volume.root_entries) return 0;
            if (entry[0] == 0) return 0;
            if (entry[0] == 0xE5u ||
                (entry[11] & FAT12_ATTR_LONG_NAME) == FAT12_ATTR_LONG_NAME ||
                (entry[11] & (FAT12_ATTR_DIRECTORY | FAT12_ATTR_VOLUME)))
                continue;
            if (memcmp(entry, short_name, 11) == 0) {
                memcpy(result, entry, 32);
                if (sector_lba) *sector_lba = volume.root_start + sector;
                if (sector_offset) *sector_offset = offset;
                return 1;
            }
        }
    }
    return 0;
}

static int read_fat_entry(uint16_t cluster, uint16_t *value) {
    uint32_t fat_offset = cluster + cluster / 2u;
    uint32_t sector_index = fat_offset / FAT12_SECTOR_SIZE;
    uint32_t offset = fat_offset % FAT12_SECTOR_SIZE;
    uint8_t low;
    uint8_t high;

    if (sector_index >= volume.fat_sectors ||
        !read_volume_sector(volume.fat_start + sector_index, sector_buffer))
        return 0;
    low = sector_buffer[offset];
    if (offset == FAT12_SECTOR_SIZE - 1u) {
        if (sector_index + 1u >= volume.fat_sectors ||
            !read_volume_sector(volume.fat_start + sector_index + 1u,
                                sector_buffer))
            return 0;
        high = sector_buffer[0];
    } else {
        high = sector_buffer[offset + 1u];
    }
    *value = (cluster & 1u)
           ? (uint16_t)(((uint16_t)low | ((uint16_t)high << 8)) >> 4)
           : (uint16_t)(((uint16_t)low | ((uint16_t)high << 8)) & 0x0FFFu);
    return 1;
}

static int write_fat_byte(uint32_t fat_copy, uint32_t offset, uint8_t value) {
    uint32_t sector = offset / FAT12_SECTOR_SIZE;
    if (sector >= volume.fat_sectors ||
        !read_volume_sector(volume.fat_start +
                            fat_copy * volume.fat_sectors + sector,
                            sector_buffer))
        return 0;
    sector_buffer[offset % FAT12_SECTOR_SIZE] = value;
    return ata_write_sectors(volume.fat_start +
                             fat_copy * volume.fat_sectors + sector,
                             1, sector_buffer);
}

static int write_fat_entry(uint16_t cluster, uint16_t value) {
    uint32_t offset = cluster + cluster / 2u;
    uint8_t low;
    uint8_t high;
    value &= 0x0FFFu;
    for (uint32_t copy = 0; copy < volume.fat_count; copy++) {
        uint8_t original_low;
        uint8_t original_high;
        uint32_t low_lba = volume.fat_start +
                           copy * volume.fat_sectors +
                           offset / FAT12_SECTOR_SIZE;
        uint32_t low_offset = offset % FAT12_SECTOR_SIZE;
        uint32_t high_lba = volume.fat_start +
                            copy * volume.fat_sectors +
                            (offset + 1u) / FAT12_SECTOR_SIZE;
        uint32_t high_offset = (offset + 1u) % FAT12_SECTOR_SIZE;
        if (!read_volume_sector(low_lba, sector_buffer)) return 0;
        original_low = sector_buffer[low_offset];
        if (!read_volume_sector(high_lba, sector_buffer)) return 0;
        original_high = sector_buffer[high_offset];
        if (cluster & 1u) {
            low = (uint8_t)((original_low & 0x0Fu) | (value << 4));
            high = (uint8_t)(value >> 4);
        } else {
            low = (uint8_t)value;
            high = (uint8_t)((original_high & 0xF0u) | (value >> 8));
        }
        if (!write_fat_byte(copy, offset, low) ||
            !write_fat_byte(copy, offset + 1u, high))
            return 0;
    }
    return 1;
}

static int write_root_entry(const fat12_file_t *file) {
    uint8_t entry[32];
    uint32_t lba;
    uint32_t offset;
    int found = find_root_entry(file->short_name, entry, &lba, &offset);
    if (found != 1 || !read_volume_sector(lba, sector_buffer)) return 0;
    sector_buffer[offset + 26u] = (uint8_t)file->first_cluster;
    sector_buffer[offset + 27u] = (uint8_t)(file->first_cluster >> 8);
    sector_buffer[offset + 28u] = (uint8_t)file->size;
    sector_buffer[offset + 29u] = (uint8_t)(file->size >> 8);
    sector_buffer[offset + 30u] = (uint8_t)(file->size >> 16);
    sector_buffer[offset + 31u] = (uint8_t)(file->size >> 24);
    return ata_write_sectors(lba, 1, sector_buffer);
}

int fat12_read_file(const char *name, void *buffer, uint32_t capacity,
                    uint32_t *file_size) {
    fat12_file_t file;
    uint32_t count;
    int open_result;
    int read_result;
    if (!buffer || !file_size) return -FAT12_ERR_INVALID;
    open_result = fat12_file_open(name, FAT12_OPEN_READ, &file);
    if (open_result != 1) return open_result;
    if (file.size > capacity) {
        fat12_file_close(&file);
        return -FAT12_ERR_INVALID;
    }
    read_result = fat12_file_read(&file, buffer, file.size, &count);
    if (read_result != 1 || count != file.size) {
        fat12_file_close(&file);
        return read_result != 1 ? read_result : -FAT12_ERR_IO;
    }
    *file_size = count;
    return fat12_file_close(&file);
}

static int create_root_entry(const uint8_t short_name[11]) {
    uint32_t entries_read = 0;
    for (uint32_t sector = 0; sector < volume.root_sectors; sector++) {
        if (!read_volume_sector(volume.root_start + sector, sector_buffer))
            return -1;
        for (uint32_t offset = 0; offset < FAT12_SECTOR_SIZE; offset += 32u) {
            uint8_t entry[32];
            uint8_t first = sector_buffer[offset];
            if (entries_read++ >= volume.root_entries) return 0;
            if (first != 0 && first != 0xE5u) continue;
            memset(entry, 0, sizeof(entry));
            memcpy(entry, short_name, 11);
            entry[11] = FAT12_ATTR_ARCHIVE;
            memcpy(sector_buffer + offset, entry, sizeof(entry));
            return ata_write_sectors(volume.root_start + sector, 1,
                                     sector_buffer) ? 1 : -1;
        }
    }
    return 0;
}

static int refresh_file(fat12_file_t *file) {
    uint8_t entry[32];
    int found = find_root_entry(file->short_name, entry, 0, 0);
    if (found < 0) return -FAT12_ERR_IO;
    if (found != 1) return -FAT12_ERR_NOT_FOUND;
    file->first_cluster = read_u16(entry + 26);
    file->size = read_u32(entry + 28);
    return 1;
}

static int zero_cluster(uint16_t cluster) {
    static uint8_t zero_sector[FAT12_SECTOR_SIZE];
    uint32_t first_lba;
    if (cluster < 2u || cluster >= volume.cluster_count + 2u) return 0;
    first_lba = volume.data_start +
                (uint32_t)(cluster - 2u) * volume.sectors_per_cluster;
    for (uint32_t i = 0; i < volume.sectors_per_cluster; i++)
        if (!ata_write_sectors(first_lba + i, 1, zero_sector)) return 0;
    return 1;
}

static int allocate_cluster(uint16_t *result) {
    for (uint32_t candidate = 2; candidate < volume.cluster_count + 2u;
         candidate++) {
        uint16_t value;
        if (!read_fat_entry((uint16_t)candidate, &value))
            return -FAT12_ERR_IO;
        if (value != FAT12_CLUSTER_FREE) continue;
        if (!zero_cluster((uint16_t)candidate) ||
            !write_fat_entry((uint16_t)candidate, FAT12_CLUSTER_END))
            return -FAT12_ERR_IO;
        *result = (uint16_t)candidate;
        return 1;
    }
    return -FAT12_ERR_NO_SPACE;
}

static int free_cluster_chain(uint16_t first) {
    uint32_t iterations = 0;
    uint16_t current = first;
    while (current >= 2u && current < volume.cluster_count + 2u) {
        uint16_t following;
        if (iterations++ >= volume.cluster_count ||
            !read_fat_entry(current, &following) ||
            !write_fat_entry(current, FAT12_CLUSTER_FREE))
            return 0;
        if (following >= FAT12_CLUSTER_EOC) return 1;
        if (following == FAT12_CLUSTER_BAD || following < 2u) return 0;
        current = following;
    }
    return first == 0;
}

static int file_cluster_at(fat12_file_t *file, uint32_t index, int create,
                           uint16_t *result) {
    uint16_t cluster = file->first_cluster;
    uint32_t iterations = 0;
    int allocated;
    if (cluster == 0) {
        if (!create) return -FAT12_ERR_IO;
        allocated = allocate_cluster(&cluster);
        if (allocated != 1) return allocated;
        file->first_cluster = cluster;
    }
    for (uint32_t i = 0; i < index; i++) {
        uint16_t following;
        if (cluster < 2u || cluster >= volume.cluster_count + 2u ||
            iterations++ >= volume.cluster_count)
            return -FAT12_ERR_IO;
        if (!read_fat_entry(cluster, &following)) return -FAT12_ERR_IO;
        if (following >= FAT12_CLUSTER_EOC) {
            if (!create) return -FAT12_ERR_IO;
            allocated = allocate_cluster(&following);
            if (allocated != 1) return allocated;
            if (!write_fat_entry(cluster, following)) return -FAT12_ERR_IO;
        } else if (following == FAT12_CLUSTER_BAD || following < 2u ||
                   following >= volume.cluster_count + 2u) {
            return -FAT12_ERR_IO;
        }
        cluster = following;
    }
    *result = cluster;
    return 1;
}

int fat12_file_open(const char *name, uint8_t flags, fat12_file_t *file) {
    uint8_t short_name[11];
    uint8_t entry[32];
    int found;
    if (!volume.mounted || !file || !make_short_name(name, short_name) ||
        !(flags & (FAT12_OPEN_READ | FAT12_OPEN_WRITE)) ||
        ((flags & FAT12_OPEN_TRUNC) && !(flags & FAT12_OPEN_WRITE)))
        return -FAT12_ERR_INVALID;
    memset(file, 0, sizeof(*file));
    found = find_root_entry(short_name, entry, 0, 0);
    if (found < 0) return -FAT12_ERR_IO;
    if (!found) {
        int created;
        if (!(flags & FAT12_OPEN_CREATE)) return -FAT12_ERR_NOT_FOUND;
        created = create_root_entry(short_name);
        if (created < 0) return -FAT12_ERR_IO;
        if (!created) return -FAT12_ERR_NO_SPACE;
        found = find_root_entry(short_name, entry, 0, 0);
        if (found != 1) return -FAT12_ERR_IO;
    } else if ((flags & FAT12_OPEN_CREATE) && (flags & FAT12_OPEN_EXCL)) {
        return -FAT12_ERR_EXISTS;
    }
    memcpy(file->short_name, short_name, sizeof(short_name));
    file->first_cluster = read_u16(entry + 26);
    file->size = read_u32(entry + 28);
    file->flags = flags;
    file->opened = 1;
    if (flags & FAT12_OPEN_TRUNC) {
        if (!free_cluster_chain(file->first_cluster)) {
            file->opened = 0;
            return -FAT12_ERR_IO;
        }
        file->first_cluster = 0;
        file->size = 0;
        if (!write_root_entry(file)) {
            file->opened = 0;
            return -FAT12_ERR_IO;
        }
    }
    if (flags & FAT12_OPEN_APPEND) file->position = file->size;
    return 1;
}

int fat12_file_close(fat12_file_t *file) {
    if (!file || !file->opened) return -FAT12_ERR_INVALID;
    file->opened = 0;
    return 1;
}

int fat12_file_read(fat12_file_t *file, void *buffer, uint32_t count,
                    uint32_t *read_count) {
    uint8_t *destination = (uint8_t *)buffer;
    uint32_t copied = 0;
    uint32_t bytes_per_cluster;
    int refreshed;
    if (!read_count) return -FAT12_ERR_INVALID;
    *read_count = 0;
    if (!file || !file->opened || !(file->flags & FAT12_OPEN_READ) ||
        (!buffer && count))
        return -FAT12_ERR_INVALID;
    refreshed = refresh_file(file);
    if (refreshed != 1) return refreshed;
    if (file->position >= file->size || count == 0) return 1;
    if (count > file->size - file->position)
        count = file->size - file->position;
    bytes_per_cluster =
        (uint32_t)volume.sectors_per_cluster * FAT12_SECTOR_SIZE;

    while (copied < count) {
        uint32_t absolute = file->position + copied;
        uint32_t cluster_index = absolute / bytes_per_cluster;
        uint32_t within_cluster = absolute % bytes_per_cluster;
        uint32_t sector_index = within_cluster / FAT12_SECTOR_SIZE;
        uint32_t sector_offset = within_cluster % FAT12_SECTOR_SIZE;
        uint32_t amount = FAT12_SECTOR_SIZE - sector_offset;
        uint16_t cluster;
        uint32_t lba;
        int located;
        if (amount > count - copied) amount = count - copied;
        located = file_cluster_at(file, cluster_index, 0, &cluster);
        if (located != 1) {
            file->position += copied;
            *read_count = copied;
            return copied ? 1 : located;
        }
        lba = volume.data_start +
              (uint32_t)(cluster - 2u) * volume.sectors_per_cluster +
              sector_index;
        if (!read_volume_sector(lba, sector_buffer)) break;
        memcpy(destination + copied, sector_buffer + sector_offset, amount);
        copied += amount;
    }
    file->position += copied;
    *read_count = copied;
    return copied == count ? 1 : -FAT12_ERR_IO;
}

static int write_file_bytes(fat12_file_t *file, uint32_t position,
                            const uint8_t *source, uint32_t count,
                            uint32_t *written_out) {
    uint32_t copied = 0;
    uint32_t bytes_per_cluster =
        (uint32_t)volume.sectors_per_cluster * FAT12_SECTOR_SIZE;
    *written_out = 0;
    while (copied < count) {
        uint32_t absolute = position + copied;
        uint32_t cluster_index = absolute / bytes_per_cluster;
        uint32_t within_cluster = absolute % bytes_per_cluster;
        uint32_t sector_index = within_cluster / FAT12_SECTOR_SIZE;
        uint32_t sector_offset = within_cluster % FAT12_SECTOR_SIZE;
        uint32_t amount = FAT12_SECTOR_SIZE - sector_offset;
        uint16_t cluster;
        uint32_t lba;
        int located;
        if (amount > count - copied) amount = count - copied;
        located = file_cluster_at(file, cluster_index, 1, &cluster);
        if (located != 1) {
            *written_out = copied;
            return located;
        }
        lba = volume.data_start +
              (uint32_t)(cluster - 2u) * volume.sectors_per_cluster +
              sector_index;
        if (sector_offset != 0 || amount != FAT12_SECTOR_SIZE) {
            if (!read_volume_sector(lba, sector_buffer)) break;
        }
        memcpy(sector_buffer + sector_offset, source + copied, amount);
        if (!ata_write_sectors(lba, 1, sector_buffer)) break;
        copied += amount;
    }
    *written_out = copied;
    return copied == count ? 1 : -FAT12_ERR_IO;
}

int fat12_file_write(fat12_file_t *file, const void *buffer, uint32_t count,
                     uint32_t *write_count) {
    static const uint8_t zeros[FAT12_SECTOR_SIZE];
    const uint8_t *source = (const uint8_t *)buffer;
    uint32_t written = 0;
    uint32_t original_position;
    int status = 1;
    int refreshed;
    if (!write_count) return -FAT12_ERR_INVALID;
    *write_count = 0;
    if (!file || !file->opened || !(file->flags & FAT12_OPEN_WRITE) ||
        (!buffer && count))
        return -FAT12_ERR_INVALID;
    if (count == 0) return 1;
    refreshed = refresh_file(file);
    if (refreshed != 1) return refreshed;
    if (file->flags & FAT12_OPEN_APPEND) file->position = file->size;
    if (count > 0xFFFFFFFFu - file->position) return -FAT12_ERR_INVALID;
    original_position = file->position;
    if (original_position > file->size) {
        uint32_t gap = original_position - file->size;
        uint32_t zeroed = 0;
        while (zeroed < gap) {
            uint32_t amount = gap - zeroed;
            uint32_t done = 0;
            int filled;
            if (amount > sizeof(zeros)) amount = sizeof(zeros);
            filled = write_file_bytes(file, file->size + zeroed,
                                      zeros, amount, &done);
            zeroed += done;
            if (filled != 1) {
                status = filled;
                break;
            }
        }
        if (status == 1) {
            file->size = original_position;
            if (!write_root_entry(file)) status = -FAT12_ERR_IO;
        }
    }
    if (status == 1 && count) {
        uint32_t done = 0;
        status = write_file_bytes(file, original_position, source, count,
                                  &done);
        written = done;
    }
    if (original_position + written > file->size)
        file->size = original_position + written;
    file->position = original_position + written;
    if (!write_root_entry(file)) status = -FAT12_ERR_IO;
    *write_count = written;
    if (status != 1) return written ? 1 : status;
    return written == count ? 1 : -FAT12_ERR_IO;
}

int fat12_file_seek(fat12_file_t *file, int32_t offset, uint32_t whence,
                    uint32_t *new_position) {
    uint32_t base;
    uint32_t amount;
    uint32_t target;
    int refreshed;
    if (!new_position) return -FAT12_ERR_INVALID;
    if (!file || !file->opened) return -FAT12_ERR_INVALID;
    refreshed = refresh_file(file);
    if (refreshed != 1) return refreshed;
    if (whence == FAT12_SEEK_SET) base = 0;
    else if (whence == FAT12_SEEK_CUR) base = file->position;
    else if (whence == FAT12_SEEK_END) base = file->size;
    else return -FAT12_ERR_INVALID;
    if (offset < 0) {
        amount = (uint32_t)(-(offset + 1)) + 1u;
        if (base < amount) return -FAT12_ERR_INVALID;
        target = base - amount;
    } else {
        amount = (uint32_t)offset;
        if (base > 0xFFFFFFFFu - amount) return -FAT12_ERR_INVALID;
        target = base + amount;
    }
    if (target > 0x7FFFFFFFu) return -FAT12_ERR_INVALID;
    file->position = target;
    *new_position = target;
    return 1;
}
