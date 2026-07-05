#include "partition.h"
#include "device_validate.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <parted/exception.h>

// Context structure for partition operations
struct winafi_partition_ctx {
    int dummy;  // Placeholder for future state
};

// Sector size constants
#define SECTOR_SIZE 512
#define BYTES_PER_MB (1024 * 1024)

// Partition alignment: 1MB = 2048 sectors (1024*1024/512)
#define PARTITION_ALIGNMENT_SECTORS 2048

// Minimum sizes in sectors
#define MIN_BOOT_SIZE_SECTORS (50 * BYTES_PER_MB / SECTOR_SIZE)   // 50MB minimum
#define MAX_BOOT_SIZE_SECTORS (200 * BYTES_PER_MB / SECTOR_SIZE)  // 200MB maximum

// MBR maximum partitions
#define MBR_MAX_PARTITIONS 4

// Minimum device size for partition planning (10MB)
#define MIN_DEVICE_SIZE (10 * BYTES_PER_MB)

// Exception handler for libparted
static PedExceptionOption exception_handler(PedException *exception) {
    (void)exception;  // Suppress unused warning
    return PED_EXCEPTION_UNHANDLED;
}

// Plan MBR partition table (no device access yet)
// Implements 1MB (2048 sectors) alignment for modern storage
// References:
//   - https://www.diskgenius.com/how-to/4k-alignment.html
//   - https://rainbow.chard.org/2013/01/30/how-to-align-partitions-for-best-performance-using-parted/
int plan_mbr_partition(
    uint64_t device_size_bytes,
    int num_partitions,
    partition_plan_t *out_plan
) {
    // Validate inputs
    if (out_plan == NULL) {
        return PARTITION_ERR_LIBPARTED;
    }

    if (num_partitions < 1 || num_partitions > MBR_MAX_PARTITIONS) {
        return PARTITION_ERR_INVALID_SIZE;
    }

    if (device_size_bytes < MIN_DEVICE_SIZE) {
        return PARTITION_ERR_NO_SPACE;
    }

    // Initialize the plan
    memset(out_plan, 0, sizeof(partition_plan_t));
    out_plan->table_type = PARTITION_MBR;
    out_plan->num_partitions = num_partitions;

    // Calculate total sectors
    uint64_t total_sectors = device_size_bytes / SECTOR_SIZE;

    // Reserve space: MBR (1 sector) + alignment + backup space
    uint64_t reserved_sectors = PARTITION_ALIGNMENT_SECTORS + 2048;
    if (total_sectors <= reserved_sectors) {
        return PARTITION_ERR_NO_SPACE;
    }

    // Available sectors for partitions
    uint64_t available_sectors = total_sectors - reserved_sectors;
    uint64_t partition_size = available_sectors / (uint64_t)num_partitions;

    // Each partition needs at least 1MB
    if (partition_size < PARTITION_ALIGNMENT_SECTORS) {
        return PARTITION_ERR_NO_SPACE;
    }

    // Create partition plans with equal sizing
    for (int i = 0; i < num_partitions; i++) {
        partition_info_t *part = &out_plan->partitions[i];

        // Start sector: aligned to 1MB
        part->start_sector = PARTITION_ALIGNMENT_SECTORS + ((uint64_t)i * partition_size);

        // End sector: next partition start - 1 (except last partition)
        if (i == num_partitions - 1) {
            // Last partition: use all remaining space
            part->end_sector = total_sectors - 1;
        } else {
            part->end_sector = part->start_sector + partition_size - 1;
        }

        // First partition is bootable
        part->boot = (i == 0) ? 1 : 0;

        // Default partition type: 0x83 (Linux)
        part->partition_type = 0x83;
    }

    return PARTITION_OK;
}

// Plan GPT partition table
// Implements 1MB (2048 sectors) alignment for modern storage
int plan_gpt_partition(
    uint64_t device_size_bytes,
    int num_partitions,
    partition_plan_t *out_plan
) {
    // Validate inputs
    if (out_plan == NULL) {
        return PARTITION_ERR_LIBPARTED;
    }

    if (num_partitions < 1) {
        return PARTITION_ERR_INVALID_SIZE;
    }

    if (device_size_bytes < MIN_DEVICE_SIZE) {
        return PARTITION_ERR_NO_SPACE;
    }

    // Initialize the plan
    memset(out_plan, 0, sizeof(partition_plan_t));
    out_plan->table_type = PARTITION_GPT;
    out_plan->num_partitions = num_partitions;

    // Calculate total sectors
    uint64_t total_sectors = device_size_bytes / SECTOR_SIZE;

    // Reserve space: GPT headers + alignment
    // GPT: 34 sectors at start + 33 sectors at end + alignment
    uint64_t reserved_sectors = 34 + 33 + PARTITION_ALIGNMENT_SECTORS;
    if (total_sectors <= reserved_sectors) {
        return PARTITION_ERR_NO_SPACE;
    }

    // Available sectors for partitions (after GPT header)
    uint64_t first_usable = 34 + PARTITION_ALIGNMENT_SECTORS;
    uint64_t available_sectors = total_sectors - first_usable - 33;
    uint64_t partition_size = available_sectors / (uint64_t)num_partitions;

    // Each partition needs at least 1MB
    if (partition_size < PARTITION_ALIGNMENT_SECTORS) {
        return PARTITION_ERR_NO_SPACE;
    }

    // Create partition plans with equal sizing
    for (int i = 0; i < num_partitions; i++) {
        partition_info_t *part = &out_plan->partitions[i];

        // Start sector: after GPT header + alignment
        part->start_sector = first_usable + ((uint64_t)i * partition_size);

        // End sector
        if (i == num_partitions - 1) {
            // Last partition: use space before backup GPT
            part->end_sector = total_sectors - 34 - 1;
        } else {
            part->end_sector = part->start_sector + partition_size - 1;
        }

        // GPT doesn't use boot flag, but mark first as ESP if needed
        part->boot = 0;
        part->partition_type = 0x83;
    }

    return PARTITION_OK;
}

// Initialize partition context
winafi_partition_ctx_t *partition_init(void) {
    winafi_partition_ctx_t *ctx = (winafi_partition_ctx_t *)malloc(sizeof(winafi_partition_ctx_t));
    if (ctx == NULL) {
        return NULL;
    }
    ctx->dummy = 0;
    return ctx;
}

// Cleanup partition context
void partition_cleanup(winafi_partition_ctx_t *ctx) {
    if (ctx != NULL) {
        free(ctx);
    }
}

// Calculate partition layout for a device
// boot_size_bytes: requested boot partition size (e.g., 100MB)
// total_sectors: total device size in sectors
// Returns: 0 on success, negative on error
int partition_calculate_layout(uint64_t boot_size_bytes,
                              uint64_t total_sectors,
                              partition_layout_t *out_layout) {
    if (out_layout == NULL) {
        return -1;
    }

    // Convert boot size from bytes to sectors
    uint64_t boot_size_sectors = boot_size_bytes / SECTOR_SIZE;

    // Validate boot partition size
    if (boot_size_sectors < MIN_BOOT_SIZE_SECTORS) {
        boot_size_sectors = MIN_BOOT_SIZE_SECTORS;
    }
    if (boot_size_sectors > MAX_BOOT_SIZE_SECTORS) {
        boot_size_sectors = MAX_BOOT_SIZE_SECTORS;
    }

    // Verify we have space for both partitions (boot + data)
    // Add 1 sector for MBR and some alignment
    uint64_t min_required = boot_size_sectors + 2048;  // 1MB alignment
    if (total_sectors < min_required) {
        return -1;  // Device too small
    }

    // Calculate data partition
    // Data starts after boot partition + alignment
    uint64_t data_start = boot_size_sectors + 2048;
    uint64_t data_size = total_sectors - data_start;

    // Populate output structure
    out_layout->boot_size = boot_size_sectors * SECTOR_SIZE;
    out_layout->data_start = data_start;
    out_layout->data_size = data_size;

    return 0;
}

// Create partition table on device (REQUIRES ROOT)
// device: /dev/sdX (block device, not partition)
// total_sectors: total device size in sectors
// boot_size_bytes: size of FAT32 boot partition
// Returns: 0 on success, negative on error
int partition_wipe_and_create(const char *device,
                             uint64_t total_sectors,
                             uint64_t boot_size_bytes) {
    if (device == NULL) {
        return -1;
    }

    // Final safety guard before any destructive write
    if (final_wipe_guard(device) != VALIDATE_OK) {
        return -1;
    }

    // First, calculate the layout
    partition_layout_t layout;
    if (partition_calculate_layout(boot_size_bytes, total_sectors, &layout) != 0) {
        return -1;
    }

    // Initialize libparted
    ped_exception_set_handler(exception_handler);

    // Get the device
    PedDevice *ped_dev = ped_device_get(device);
    if (ped_dev == NULL) {
        return -1;
    }

    // Get disk type for MBR (msdos)
    PedDiskType *disk_type = ped_disk_type_get("msdos");
    if (disk_type == NULL) {
        ped_device_destroy(ped_dev);
        return -1;
    }

    // Create new MBR partition table
    PedDisk *disk = ped_disk_new_fresh(ped_dev, disk_type);
    if (disk == NULL) {
        ped_device_destroy(ped_dev);
        return -1;
    }

    // Create constraints for partitions
    PedConstraint *constraint = ped_device_get_constraint(ped_dev);
    if (constraint == NULL) {
        ped_disk_destroy(disk);
        ped_device_destroy(ped_dev);
        return -1;
    }

    // Create FAT32 boot partition (partition 1)
    // Start at sector 2048 (1MB alignment)
    PedSector boot_start = 2048;
    PedSector boot_end = boot_start + (long long)layout.boot_size / SECTOR_SIZE - 1;

    PedGeometry boot_geom;
    if (!ped_geometry_init(&boot_geom, ped_dev, boot_start,
                          (long long)layout.boot_size / SECTOR_SIZE)) {
        ped_constraint_destroy(constraint);
        ped_disk_destroy(disk);
        ped_device_destroy(ped_dev);
        return -1;
    }

    // Get FAT32 file system type
    PedFileSystemType *fat32_type = ped_file_system_type_get("fat32");
    if (fat32_type == NULL) {
        ped_constraint_destroy(constraint);
        ped_disk_destroy(disk);
        ped_device_destroy(ped_dev);
        return -1;  // E-21-D (Format Missing Tool)
    }

    PedPartition *boot_part = ped_partition_new(disk, PED_PARTITION_NORMAL,
                                                 fat32_type,
                                                 boot_start, boot_end);
    if (boot_part == NULL) {
        ped_constraint_destroy(constraint);
        ped_disk_destroy(disk);
        ped_device_destroy(ped_dev);
        return -1;
    }

    // Add partition to disk - disk owns boot_part after success
    if (ped_disk_add_partition(disk, boot_part, constraint) == 0) {
        // FAILED - must cleanup the partition we created
        ped_partition_destroy(boot_part);
        ped_constraint_destroy(constraint);
        ped_disk_destroy(disk);
        ped_device_destroy(ped_dev);
        return -1;  // E-20-B
    }
    // SUCCESS - DO NOT destroy boot_part, disk owns it now

    // Create NTFS data partition (partition 2)
    // Start after boot partition
    PedSector data_start = boot_end + 1;
    PedSector data_end = ped_dev->length - 1;

    PedGeometry data_geom;
    if (!ped_geometry_init(&data_geom, ped_dev, data_start, data_end - data_start + 1)) {
        ped_constraint_destroy(constraint);
        ped_disk_destroy(disk);
        ped_device_destroy(ped_dev);
        return -1;
    }

    // Get NTFS file system type
    PedFileSystemType *ntfs_type = ped_file_system_type_get("ntfs");
    if (ntfs_type == NULL) {
        ped_constraint_destroy(constraint);
        ped_disk_destroy(disk);
        ped_device_destroy(ped_dev);
        return -1;  // E-21-D (Format Missing Tool)
    }

    PedPartition *data_part = ped_partition_new(disk, PED_PARTITION_NORMAL,
                                                 ntfs_type,
                                                 data_start, data_end);
    if (data_part == NULL) {
        ped_constraint_destroy(constraint);
        ped_disk_destroy(disk);
        ped_device_destroy(ped_dev);
        return -1;
    }

    // Add partition to disk - disk owns data_part after success
    if (ped_disk_add_partition(disk, data_part, constraint) == 0) {
        // FAILED - must cleanup the partition we created
        ped_partition_destroy(data_part);
        ped_constraint_destroy(constraint);
        ped_disk_destroy(disk);
        ped_device_destroy(ped_dev);
        return -1;  // E-20-B
    }
    // SUCCESS - DO NOT destroy data_part, disk owns it now

    // Set boot flag on FAT32 partition
    if (ped_partition_set_flag(boot_part, PED_PARTITION_BOOT, 1) == 0) {
        ped_constraint_destroy(constraint);
        ped_disk_destroy(disk);
        ped_device_destroy(ped_dev);
        return -1;
    }

    // Commit changes to disk
    if (ped_disk_commit(disk) == 0) {
        ped_constraint_destroy(constraint);
        ped_disk_destroy(disk);
        ped_device_destroy(ped_dev);
        return -1;
    }

    // Cleanup
    ped_constraint_destroy(constraint);
    ped_disk_destroy(disk);
    ped_device_destroy(ped_dev);

    return 0;
}

// Set partition flags (boot, esp)
int partition_set_boot_flag(const char *device, int partition_number) {
    if (device == NULL || partition_number < 1) {
        return -1;
    }

    // Initialize libparted
    ped_exception_set_handler(exception_handler);

    // Get the device
    PedDevice *ped_dev = ped_device_get(device);
    if (ped_dev == NULL) {
        return -1;
    }

    // Get existing disk
    PedDisk *disk = ped_disk_new(ped_dev);
    if (disk == NULL) {
        ped_device_destroy(ped_dev);
        return -1;
    }

    // Find partition
    PedPartition *part = ped_disk_get_partition(disk, partition_number);
    if (part == NULL) {
        ped_disk_destroy(disk);
        ped_device_destroy(ped_dev);
        return -1;
    }

    // Set boot flag
    if (ped_partition_set_flag(part, PED_PARTITION_BOOT, 1) == 0) {
        ped_disk_destroy(disk);
        ped_device_destroy(ped_dev);
        return -1;
    }

    // Commit changes
    if (ped_disk_commit(disk) == 0) {
        ped_disk_destroy(disk);
        ped_device_destroy(ped_dev);
        return -1;
    }

    // Cleanup
    ped_disk_destroy(disk);
    ped_device_destroy(ped_dev);

    return 0;
}
