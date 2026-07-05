#define _DEFAULT_SOURCE
#include "device_validate.h"
#include <errno.h>
#include <fcntl.h>
#include <mntent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <dirent.h>
#include <limits.h>
#include <linux/limits.h>

static int read_first_line(const char *path, char *buf, size_t bufsz)
{
    FILE *fp = fopen(path, "r");
    if (!fp) return -1;
    if (!fgets(buf, (int)bufsz, fp)) {
        fclose(fp);
        return -1;
    }
    fclose(fp);
    size_t len = strlen(buf);
    if (len > 0 && buf[len - 1] == '\n') buf[len - 1] = '\0';
    return 0;
}

static int devnode_to_sysname(const char *devnode, char *sysname, size_t sysname_size)
{
    if (!devnode || !sysname || sysname_size == 0) return -1;
    struct stat st;
    if (stat(devnode, &st) != 0 || !S_ISBLK(st.st_mode)) return -1;

    char path[PATH_MAX];
    snprintf(path, sizeof(path), "/sys/dev/block/%u:%u",
             major(st.st_rdev), minor(st.st_rdev));

    char resolved[PATH_MAX];
    if (!realpath(path, resolved)) return -1;
    const char *base = strrchr(resolved, '/');
    if (!base || !base[1]) return -1;
    snprintf(sysname, sysname_size, "%s", base + 1);
    return 0;
}

static int read_removable_flag(const char *sysname)
{
    char path[PATH_MAX], value[32];
    snprintf(path, sizeof(path), "/sys/block/%s/removable", sysname);
    if (read_first_line(path, value, sizeof(value)) != 0) return 0;
    return strcmp(value, "1") == 0;
}

static int has_usb_ancestor(const char *sysname)
{
    char path[PATH_MAX], resolved[PATH_MAX];
    snprintf(path, sizeof(path), "/sys/block/%s", sysname);
    if (!realpath(path, resolved)) return 0;
    return strstr(resolved, "/usb") != NULL;
}

static int mount_entry_matches_dev(const char *mnt_fsname, dev_t dev)
{
    struct stat st;
    if (!mnt_fsname || stat(mnt_fsname, &st) != 0 || !S_ISBLK(st.st_mode)) {
        return 0;
    }
    return st.st_rdev == dev;
}

static int is_protected_mountpoint(const char *mountpoint)
{
    const char *protected_mounts[] = {
        "/", "/boot", "/boot/efi", "/efi",
        "/home", "/var", "/usr"
    };
    for (size_t i = 0; i < sizeof(protected_mounts) / sizeof(protected_mounts[0]); i++) {
        if (strcmp(mountpoint, protected_mounts[i]) == 0) return 1;
    }
    return 0;
}

static int child_partition_has_swap(const char *sysname)
{
    // Collect all partition devnums for this parent disk
    dev_t devs[128];
    int dev_count = 0;

    char block_dir[PATH_MAX];
    snprintf(block_dir, sizeof(block_dir), "/sys/block/%s", sysname);

    DIR *dp = opendir(block_dir);
    if (!dp) return 0;

    struct dirent *de;
    while ((de = readdir(dp)) != NULL && dev_count < 128) {
        if (de->d_name[0] == '.') continue;
        char part_attr[PATH_MAX];
        if (snprintf(part_attr, sizeof(part_attr), "%s/%s/partition",
                     block_dir, de->d_name) < 0 ||
            strlen(block_dir) + 1 + strlen(de->d_name) + strlen("/partition") >= sizeof(part_attr)) {
            continue;
        }
        if (access(part_attr, F_OK) != 0) continue;
        char part_dev[PATH_MAX];
        if (snprintf(part_dev, sizeof(part_dev), "/dev/%s", de->d_name) < 0 ||
            strlen("/dev/") + strlen(de->d_name) >= sizeof(part_dev)) {
            continue;
        }
        struct stat st;
        if (stat(part_dev, &st) == 0 && S_ISBLK(st.st_mode)) {
            devs[dev_count++] = st.st_rdev;
        }
    }
    closedir(dp);

    // Also add the parent device itself (swap can be on a whole disk)
    {
        char parent_dev[PATH_MAX];
        snprintf(parent_dev, sizeof(parent_dev), "/dev/%s", sysname);
        struct stat st;
        if (stat(parent_dev, &st) == 0 && S_ISBLK(st.st_mode)) {
            devs[dev_count++] = st.st_rdev;
        }
    }

    // Check /proc/swaps for matching devices
    FILE *fp = fopen("/proc/swaps", "r");
    if (!fp) return 0;

    char line[4096];
    // Skip header line
    if (!fgets(line, sizeof(line), fp)) {
        fclose(fp);
        return 0;
    }

    int found = 0;
    while (fgets(line, sizeof(line), fp)) {
        char filename[256];
        if (sscanf(line, "%255s", filename) != 1) continue;
        struct stat st;
        if (stat(filename, &st) != 0 || !S_ISBLK(st.st_mode)) continue;
        for (int i = 0; i < dev_count; i++) {
            if (st.st_rdev == devs[i]) {
                found = 1;
                break;
            }
        }
        if (found) break;
    }
    fclose(fp);
    return found;
}

static int child_partition_mounted_at_protected(const char *devnode)
{
    char sysname[64];
    if (devnode_to_sysname(devnode, sysname, sizeof(sysname)) != 0) {
        return VALIDATE_ERR_NOT_FOUND;
    }

    struct stat st;
    if (stat(devnode, &st) != 0 || !S_ISBLK(st.st_mode)) {
        return VALIDATE_ERR_NOT_FOUND;
    }

    char block_dir[PATH_MAX];
    snprintf(block_dir, sizeof(block_dir), "/sys/block/%s", sysname);

    dev_t devs[128];
    int dev_count = 0;
    devs[dev_count++] = st.st_rdev;

    DIR *dp = opendir(block_dir);
    if (dp) {
        struct dirent *de;
        while ((de = readdir(dp)) != NULL && dev_count < 128) {
            if (de->d_name[0] == '.') continue;
            char part_dev[PATH_MAX], part_path[PATH_MAX];
            if (snprintf(part_path, sizeof(part_path), "%s/%s/partition",
                         block_dir, de->d_name) < 0 ||
                strlen(block_dir) + 1 + strlen(de->d_name) + strlen("/partition") >= sizeof(part_path)) {
                continue;
            }
            if (access(part_path, F_OK) != 0) continue;
            if (snprintf(part_dev, sizeof(part_dev), "/dev/%s", de->d_name) < 0 ||
                strlen("/dev/") + strlen(de->d_name) >= sizeof(part_dev)) {
                continue;
            }
            if (stat(part_dev, &st) == 0 && S_ISBLK(st.st_mode)) {
                devs[dev_count++] = st.st_rdev;
            }
        }
        closedir(dp);
    }

    const char *tables[] = { "/etc/mtab", "/proc/mounts" };
    for (size_t t = 0; t < sizeof(tables) / sizeof(tables[0]); t++) {
        FILE *fp = fopen(tables[t], "r");
        if (!fp) continue;
        struct mntent *entry;
        while ((entry = getmntent(fp)) != NULL) {
            for (int i = 0; i < dev_count; i++) {
                if (mount_entry_matches_dev(entry->mnt_fsname, devs[i]) &&
                    is_protected_mountpoint(entry->mnt_dir)) {
                    fclose(fp);
                    return VALIDATE_ERR_SYSTEM_DRIVE;
                }
            }
        }
        fclose(fp);
    }

    return VALIDATE_OK;
}

/**
 * System Drive Protection Strategy
 * ================================
 *
 * Protection sources:
 * 1. /etc/mtab: Current mount table (primary method)
 *    - Standard mount information
 *    - May be unavailable in some environments
 *
 * 2. /proc/mounts: Kernel mount table (fallback method)
 *    - Kernel-managed mount information
 *    - Always available in Linux systems
 *    - More reliable in restricted environments
 *
 * 3. /proc/swaps: Active swap partitions
 *    - Parsed to detect swap-on-block-device
 *
 * 4. Protected mount points (critical system partitions):
 *    - / (root partition)
 *    - /boot (bootloader and kernel)
 *    - /boot/efi (EFI system partition — UEFI firmware boot path)
 *    - /efi (alternative EFI system partition mount)
 *    - /home (user home directories)
 *    - /var (system variable data)
 *    - /usr (user programs and libraries)
 *
 * References:
 * - [Safe remove drive vs sync](https://www.linux.org/threads/safe-remove-drive-vs-sync-command.51196/)
 * - Linux getmntent(3) documentation for mount table parsing
 * - /proc/mounts kernel interface for mount information
 */

/**
 * validate_device_capacity - Check if device has sufficient capacity for ISO
 * @device_bytes: Total capacity of the device in bytes
 * @iso_bytes: Size of the ISO image in bytes
 *
 * Returns VALIDATE_OK if device_bytes >= iso_bytes + 1GB (1024*1024*1024),
 * otherwise returns VALIDATE_ERR_DEVICE_TOO_SMALL.
 */
int validate_device_capacity(uint64_t device_bytes, uint64_t iso_bytes)
{
    const uint64_t ONE_GB = 1024ULL * 1024 * 1024;
    uint64_t required_bytes = iso_bytes + ONE_GB;

    if (device_bytes >= required_bytes) {
        return VALIDATE_OK;
    }

    return VALIDATE_ERR_DEVICE_TOO_SMALL;
}

/**
 * validate_not_system_drive - Check if device is a system drive
 * @devnode: Device path (e.g. "/dev/sda1")
 *
 * Checks if device or its partitions are mounted at protected system
 * locations or used as active swap:
 * - / (root)
 * - /boot (boot partition)
 * - /boot/efi (EFI system partition)
 * - /efi (alternative EFI system partition mount)
 * - /home (home partition)
 * - /var (system variable data)
 * - /usr (user programs and libraries)
 * - swap (any partition active as swap)
 *
 * Uses dual fallback strategy:
 * 1. Primary: /etc/mtab (current mount table)
 * 2. Fallback: /proc/mounts (kernel mount table)
 *
 * Returns VALIDATE_OK if device is not a system drive,
 * VALIDATE_ERR_SYSTEM_DRIVE if mounted at protected location or in swap,
 * VALIDATE_ERR_NOT_FOUND if device not found in mount tables.
 */
int validate_not_system_drive(const char *devnode)
{
    if (!devnode) {
        return VALIDATE_ERR_NOT_FOUND;
    }

    // Check child partitions for protected mounts
    int child_check = child_partition_mounted_at_protected(devnode);
    if (child_check == VALIDATE_ERR_SYSTEM_DRIVE) {
        return child_check;
    }

    // Check if any partition of this device is active swap
    {
        char sysname[64];
        if (devnode_to_sysname(devnode, sysname, sizeof(sysname)) == 0) {
            if (child_partition_has_swap(sysname)) {
                return VALIDATE_ERR_SYSTEM_DRIVE;
            }
        }
    }

    // Protected mountpoints that should never be formatted
    const char *protected_mounts[] = {
        "/",           // root partition
        "/boot",       // boot partition
        "/boot/efi",   // EFI system partition (UEFI)
        "/efi",        // alternative EFI system partition mount
        "/home",       // user home directories
        "/var",        // system variable data
        "/usr"         // user programs and libraries
    };
    const int num_protected = 7;

    // Method 1: Check /etc/mtab (primary method)
    FILE *fp = fopen("/etc/mtab", "r");
    if (fp) {
        struct mntent *entry;
        while ((entry = getmntent(fp)) != NULL) {
            // Check if this entry matches our device
            if (strcmp(entry->mnt_fsname, devnode) == 0) {
                // Check if mounted at a protected location
                for (int i = 0; i < num_protected; i++) {
                    if (strcmp(entry->mnt_dir, protected_mounts[i]) == 0) {
                        fclose(fp);
                        return VALIDATE_ERR_SYSTEM_DRIVE;
                    }
                }
                // Device found but not at protected location
                fclose(fp);
                return VALIDATE_OK;
            }
        }
        fclose(fp);
        // Device not found in mtab, try fallback
    }

    // Method 2: Check /proc/mounts as fallback (if /etc/mtab not available)
    FILE *proc_mounts = fopen("/proc/mounts", "r");
    if (proc_mounts) {
        struct mntent *entry;
        while ((entry = getmntent(proc_mounts)) != NULL) {
            // Check if this entry matches our device
            if (strcmp(entry->mnt_fsname, devnode) == 0) {
                // Check if mounted at a protected location
                for (int i = 0; i < num_protected; i++) {
                    if (strcmp(entry->mnt_dir, protected_mounts[i]) == 0) {
                        fclose(proc_mounts);
                        return VALIDATE_ERR_SYSTEM_DRIVE;
                    }
                }
                // Device found but not at protected location
                fclose(proc_mounts);
                return VALIDATE_OK;
            }
        }
        fclose(proc_mounts);
    }

    // Device not found in either mount table or swap (not currently active)
    return VALIDATE_ERR_NOT_FOUND;
}

/**
 * validate_device_not_locked - Check if device is in use or mounted
 * @devnode: Device path (e.g. "/dev/sdb")
 *
 * Attempts to open device with O_RDONLY | O_EXCL flags.
 * O_EXCL fails if the device is already open elsewhere (in use/mounted).
 *
 * Returns VALIDATE_OK if device can be opened exclusively,
 * VALIDATE_ERR_DEVICE_LOCKED if device is in use,
 * VALIDATE_ERR_NOT_FOUND if device does not exist.
 */
int validate_device_not_locked(const char *devnode)
{
    if (!devnode) {
        return VALIDATE_ERR_NOT_FOUND;
    }

    // Try to open device with exclusive access
    int fd = open(devnode, O_RDONLY | O_EXCL);

    if (fd >= 0) {
        // Successfully opened - device is not locked
        close(fd);
        return VALIDATE_OK;
    }

    // Check specific error codes
    if (errno == ENOENT) {
        return VALIDATE_ERR_NOT_FOUND;
    }

    if (errno == EBUSY) {
        return VALIDATE_ERR_DEVICE_LOCKED;
    }

    // For other errors (EACCES, etc.), treat as locked to be safe
    return VALIDATE_ERR_DEVICE_LOCKED;
}

int validate_device_is_removable(const char *devnode)
{
    char sysname[64];
    if (devnode_to_sysname(devnode, sysname, sizeof(sysname)) != 0) {
        return VALIDATE_ERR_NOT_FOUND;
    }
    if (read_removable_flag(sysname) || has_usb_ancestor(sysname)) {
        return VALIDATE_OK;
    }
    return VALIDATE_ERR_SYSTEM_DRIVE;
}

/**
 * final_wipe_guard - Last-mile safety check before any destructive write
 * @devnode: Device path (e.g. "/dev/sdb")
 *
 * This is a self-contained guard that does NOT rely on prior validation
 * layers. It must be called by every function that performs a destructive
 * write (partition wipe, format, etc.) immediately before the write.
 *
 * Checks:
 *  1. Virtual/unsupported device types: loop, ram, dm, zram, nbd, mapper
 *  2. Device exists and is a block device
 *  3. Device is mounted (exact-match, no prefix collision)
 *  4. Any partition of this device is mounted
 *  5. Any partition is active swap
 *  6. Device or any partition is mounted at a protected system mount point
 *     (/, /boot, /boot/efi, /efi, /home, /var, /usr)
 *
 * Returns VALIDATE_OK if safe to write, VALIDATE_ERR_* if blocked.
 */
int final_wipe_guard(const char *devnode)
{
    if (!devnode) {
        return VALIDATE_ERR_NOT_FOUND;
    }

    // 1. Block virtual/unsupported device types
    if (strncmp(devnode, "/dev/loop", 9) == 0 ||
        strncmp(devnode, "/dev/ram", 8) == 0 ||
        strncmp(devnode, "/dev/dm-", 8) == 0 ||
        strncmp(devnode, "/dev/zram", 9) == 0 ||
        strncmp(devnode, "/dev/nbd", 8) == 0) {
        return VALIDATE_ERR_SYSTEM_DRIVE;
    }
    // Block device-mapper paths
    if (strncmp(devnode, "/dev/mapper/", 12) == 0) {
        return VALIDATE_ERR_SYSTEM_DRIVE;
    }

    // 2. Device must exist and be a block device
    struct stat dev_st;
    if (stat(devnode, &dev_st) != 0 || !S_ISBLK(dev_st.st_mode)) {
        return VALIDATE_ERR_NOT_FOUND;
    }

    // 3. Check if device itself is mounted (exact match only)
    {
        FILE *fp = fopen("/proc/mounts", "r");
        if (fp) {
            char line[4096];
            size_t n = strlen(devnode);
            while (fgets(line, sizeof(line), fp)) {
                if (strncmp(line, devnode, n) == 0 &&
                    (line[n] == ' ' || line[n] == '\t' || line[n] == '\n')) {
                    fclose(fp);
                    return VALIDATE_ERR_DEVICE_LOCKED;
                }
            }
            fclose(fp);
        }
    }

    // 4-6: Check partitions of this device
    char sysname[64];
    if (devnode_to_sysname(devnode, sysname, sizeof(sysname)) != 0) {
        // Cannot resolve sysname; device may have disappeared
        return VALIDATE_ERR_NOT_FOUND;
    }

    {
        char block_dir[PATH_MAX];
        struct dirent *de;
        DIR *dp;

        snprintf(block_dir, sizeof(block_dir), "/sys/block/%s", sysname);
        dp = opendir(block_dir);
        if (dp) {
            while ((de = readdir(dp)) != NULL) {
                if (de->d_name[0] == '.') continue;

                char part_attr[PATH_MAX];
                if (snprintf(part_attr, sizeof(part_attr), "%s/%s/partition",
                             block_dir, de->d_name) < 0 ||
                    strlen(block_dir) + 1 + strlen(de->d_name) + strlen("/partition")
                        >= sizeof(part_attr)) {
                    continue;
                }
                if (access(part_attr, F_OK) != 0) continue;

                char part_dev[PATH_MAX];
                if (snprintf(part_dev, sizeof(part_dev), "/dev/%s", de->d_name) < 0 ||
                    strlen("/dev/") + strlen(de->d_name) >= sizeof(part_dev)) {
                    continue;
                }

                // 4. Check if partition is mounted
                {
                    FILE *fp = fopen("/proc/mounts", "r");
                    if (fp) {
                        char line[4096];
                        size_t pn = strlen(part_dev);
                        while (fgets(line, sizeof(line), fp)) {
                            if (strncmp(line, part_dev, pn) == 0 &&
                                (line[pn] == ' ' || line[pn] == '\t' || line[pn] == '\n')) {
                                fclose(fp);
                                closedir(dp);
                                return VALIDATE_ERR_DEVICE_LOCKED;
                            }
                        }
                        fclose(fp);
                    }
                }
            }
            closedir(dp);
        }

        // 5. Check swap
        if (child_partition_has_swap(sysname)) {
            return VALIDATE_ERR_SYSTEM_DRIVE;
        }
    }

    // 6. Check system drive (root, /boot, /boot/efi, /efi, /home, /var, /usr)
    if (validate_not_system_drive(devnode) == VALIDATE_ERR_SYSTEM_DRIVE) {
        return VALIDATE_ERR_SYSTEM_DRIVE;
    }

    return VALIDATE_OK;
}
