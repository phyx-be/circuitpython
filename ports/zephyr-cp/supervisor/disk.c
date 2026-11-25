// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Bert Outtier
//
// SPDX-License-Identifier: MIT

// CIRCUITPY on a Zephyr disk (an SD card, say) instead of flash: the first
// partition of the disk's MBR, when it is a FAT one. Boards choose the disk
// with the devicetree:
//
//     chosen {
//         circuitpython,circuitpy-disk = &sd_card;   // a zephyr,sdmmc-disk
//     };
//
// The rest of the disk is left alone: a boot loader may live before the
// partition, and a disk without a FAT first partition (a Linux card, say)
// is not formatted over.

#include <zephyr/devicetree.h>

#if DT_HAS_CHOSEN(circuitpython_circuitpy_disk)

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/storage/disk_access.h>
#include <zephyr/sys/byteorder.h>

#include "py/mpconfig.h"
#include "supervisor/flash.h"

#define CIRCUITPY_DISK DT_PROP(DT_CHOSEN(circuitpython_circuitpy_disk), disk_name)

#define MBR_SIGNATURE_OFFSET 510
#define MBR_PARTITION_OFFSET 446
#define MBR_TYPE 4
#define MBR_LBA_START 8
#define MBR_LBA_COUNT 12

static K_MUTEX_DEFINE(_mutex);
static bool _ready;
static uint32_t _start;
static uint32_t _count;

static bool _is_fat(uint8_t type) {
    switch (type) {
        case 0x01: // FAT12
        case 0x04: // FAT16, under 32 MB
        case 0x06: // FAT16
        case 0x0b: // FAT32
        case 0x0c: // FAT32, LBA
        case 0x0e: // FAT16, LBA
            return true;
        default:
            return false;
    }
}

void supervisor_flash_init(void) {
    // Sector buffers of whole cache lines, for DMA
    static uint8_t mbr[FILESYSTEM_BLOCK_SIZE] __aligned(64);
    uint32_t sectors = 0;
    uint32_t sector_size = 0;

    if (_ready) {
        return;
    }

    if (disk_access_init(CIRCUITPY_DISK) != 0 ||
        disk_access_ioctl(CIRCUITPY_DISK, DISK_IOCTL_GET_SECTOR_COUNT, &sectors) != 0 ||
        disk_access_ioctl(CIRCUITPY_DISK, DISK_IOCTL_GET_SECTOR_SIZE, &sector_size) != 0) {
        printk("CIRCUITPY: disk %s not ready\n", CIRCUITPY_DISK);
        return;
    }
    if (sector_size != FILESYSTEM_BLOCK_SIZE) {
        printk("CIRCUITPY: %u byte sectors, not %u\n", sector_size, FILESYSTEM_BLOCK_SIZE);
        return;
    }
    if (disk_access_read(CIRCUITPY_DISK, mbr, 0, 1) != 0) {
        printk("CIRCUITPY: cannot read the partition table\n");
        return;
    }
    if (sys_get_le16(&mbr[MBR_SIGNATURE_OFFSET]) != 0xaa55) {
        printk("CIRCUITPY: no partition table on %s\n", CIRCUITPY_DISK);
        return;
    }

    const uint8_t *entry = &mbr[MBR_PARTITION_OFFSET];
    uint32_t start = sys_get_le32(&entry[MBR_LBA_START]);
    uint32_t count = sys_get_le32(&entry[MBR_LBA_COUNT]);

    if (!_is_fat(entry[MBR_TYPE])) {
        printk("CIRCUITPY: first partition of type 0x%02x, not FAT\n", entry[MBR_TYPE]);
        return;
    }
    if (start == 0 || count == 0 || start >= sectors || count > sectors - start) {
        printk("CIRCUITPY: partition %u+%u outside the %u sectors\n", start, count, sectors);
        return;
    }

    _start = start;
    _count = count;
    _ready = true;
    printk("CIRCUITPY: %s sectors %u to %u\n", CIRCUITPY_DISK, start, start + count - 1);
}

uint32_t supervisor_flash_get_block_size(void) {
    return FILESYSTEM_BLOCK_SIZE;
}

uint32_t supervisor_flash_get_block_count(void) {
    return _ready ? _count : 0;
}

mp_uint_t supervisor_flash_read_blocks(uint8_t *dest, uint32_t block, uint32_t num_blocks) {
    if (!_ready || block >= _count || num_blocks > _count - block) {
        return 1;
    }
    k_mutex_lock(&_mutex, K_FOREVER);
    int ret = disk_access_read(CIRCUITPY_DISK, dest, _start + block, num_blocks);
    k_mutex_unlock(&_mutex);
    return ret == 0 ? 0 : 1;
}

mp_uint_t supervisor_flash_write_blocks(const uint8_t *src, uint32_t block, uint32_t num_blocks) {
    if (!_ready || block >= _count || num_blocks > _count - block) {
        return 1;
    }
    k_mutex_lock(&_mutex, K_FOREVER);
    int ret = disk_access_write(CIRCUITPY_DISK, src, _start + block, num_blocks);
    k_mutex_unlock(&_mutex);
    return ret == 0 ? 0 : 1;
}

// Writes go straight to the card: nothing is cached here.
void port_internal_flash_flush(void) {
    if (_ready) {
        (void)disk_access_ioctl(CIRCUITPY_DISK, DISK_IOCTL_CTRL_SYNC, NULL);
    }
}

void supervisor_flash_release_cache(void) {
    port_internal_flash_flush();
}

#endif // DT_HAS_CHOSEN(circuitpython_circuitpy_disk)
