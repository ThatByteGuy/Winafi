#define _GNU_SOURCE
#include "windows_boot.h"
#include "filesystem.h"
#include "pki.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <libgen.h>

// Filesystem type constants (from filesystem.h header)
#define FSTYPE_FAT32  1
#define FSTYPE_NTFS   2
#define FSTYPE_EXFAT  3

/**
 * Helper: Create directory with parents if needed (mkdir -p style)
 *
 * Returns: 0 on success, negative on error
 */
static int create_dir_if_needed(const char *path) {
    if (!path) return -1;

    // Try to create directory
    int ret = mkdir(path, 0755);
    if (ret == 0) {
        // Directory created successfully
        return 0;
    }

    if (errno == EEXIST) {
        // Directory already exists, check it's actually a directory
        struct stat st;
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
            return 0;
        }
        // Exists but not a directory
        return -1;
    }

    // Other error - might be parent doesn't exist, try to create parent first
    if (errno == ENOENT) {
        char *path_copy = strdup(path);
        if (!path_copy) return -1;

        char *parent = dirname(path_copy);
        if (parent && strcmp(parent, path) != 0) {
            // Recursively create parent
            ret = create_dir_if_needed(parent);
            free(path_copy);
            if (ret != 0) return ret;

            // Now try to create this directory again
            ret = mkdir(path, 0755);
            if (ret == 0 || errno == EEXIST) {
                return 0;
            }
        } else {
            free(path_copy);
        }
    }

    return -1;
}

/**
 * Helper: Copy a single file from source to destination
 *
 * Preserves file permissions and handles errors gracefully.
 * Returns: 0 on success, negative on error
 */
static int copy_file(const char *src_path, const char *dest_path) {
    if (!src_path || !dest_path) {
        return -1;
    }

    // Check source file exists
    struct stat src_stat;
    if (stat(src_path, &src_stat) != 0) {
        fprintf(stderr, "ERROR: Source file not found: %s (errno: %d)\n", src_path, errno);
        return -1;
    }

    // Check it's a regular file
    if (!S_ISREG(src_stat.st_mode)) {
        fprintf(stderr, "ERROR: Source is not a regular file: %s\n", src_path);
        return -1;
    }

    // Create destination parent directory if needed
    char *dest_copy = strdup(dest_path);
    if (!dest_copy) {
        return -1;
    }

    char *dest_dir = dirname(dest_copy);
    if (create_dir_if_needed(dest_dir) != 0) {
        fprintf(stderr, "ERROR: Failed to create destination directory: %s\n", dest_dir);
        free(dest_copy);
        return -1;
    }
    free(dest_copy);

    // Open source file for reading
    int src_fd = open(src_path, O_RDONLY);
    if (src_fd < 0) {
        fprintf(stderr, "ERROR: Failed to open source file: %s (errno: %d)\n", src_path, errno);
        return -1;
    }

    // Open destination file for writing (with O_NOFOLLOW for security)
    int dest_fd = open(dest_path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW,
                      src_stat.st_mode & 0777);
    if (dest_fd < 0) {
        fprintf(stderr, "ERROR: Failed to open destination file: %s (errno: %d)\n", dest_path, errno);
        close(src_fd);
        return -1;
    }

    // Copy file contents
    char buffer[65536];  // 64KB buffer
    ssize_t bytes_read;
    int error = 0;

    while ((bytes_read = read(src_fd, buffer, sizeof(buffer))) > 0) {
        ssize_t bytes_written = write(dest_fd, buffer, (size_t)bytes_read);
        if (bytes_written != bytes_read) {
            fprintf(stderr, "ERROR: Failed to write to destination file: %s (errno: %d)\n", dest_path, errno);
            error = -1;
            break;
        }
    }

    if (bytes_read < 0) {
        fprintf(stderr, "ERROR: Failed to read from source file: %s (errno: %d)\n", src_path, errno);
        error = -1;
    }

    close(src_fd);
    close(dest_fd);

    if (error != 0) {
        // Remove partial file on error
        unlinkat(AT_FDCWD, dest_path, AT_SYMLINK_NOFOLLOW);
        return -1;
    }

    return 0;
}

/**
 * Helper: Check if boot_info has any boot files specified
 */
static int boot_info_has_files(const windows_boot_info_t *boot_info) {
    if (!boot_info) return 0;

    return boot_info->bootmgr_path ||
           boot_info->bcd_path ||
           boot_info->bootx64_path ||
           boot_info->bootia32_path ||
           boot_info->bootaa64_path;
}

/**
 * Setup UEFI boot environment
 */
static int setup_uefi_boot(const char *mount_point,
                          const windows_boot_info_t *boot_info,
                          int filesystem_type,
                          winafi_progress_callback_t progress_cb) {
    // CRITICAL: UEFI boot requires FAT32 filesystem
    // Windows UEFI cannot read bootloader from NTFS or exFAT (RESEARCH_PHASE5.md)
    if (filesystem_type != FSTYPE_FAT32) {
        fprintf(stderr, "ERROR: UEFI boot requires FAT32 filesystem, got type %d\n", filesystem_type);
        return ISO_ERR_EXTRACT_FAILED;
    }

    // Validate that BOOTX64.EFI file exists before creating directories
    if (!boot_info->bootx64_path) {
        fprintf(stderr, "ERROR: UEFI boot requires BOOTX64.EFI path\n");
        return ISO_ERR_FILE_NOT_FOUND;
    }

    struct stat st;
    if (stat(boot_info->bootx64_path, &st) != 0) {
        fprintf(stderr, "ERROR: BOOTX64.EFI not found at %s\n", boot_info->bootx64_path);
        return ISO_ERR_FILE_NOT_FOUND;
    }

    // Create EFI/BOOT directory (only if source file exists)
    char efi_boot_dir[PATH_MAX];
    snprintf(efi_boot_dir, sizeof(efi_boot_dir), "%s/EFI/BOOT", mount_point);
    if (create_dir_if_needed(efi_boot_dir) != 0) {
        fprintf(stderr, "ERROR: Failed to create EFI/BOOT directory\n");
        return ISO_ERR_EXTRACT_FAILED;
    }

    // Copy BOOTX64.EFI
    char dest_bootx64[PATH_MAX];
    snprintf(dest_bootx64, sizeof(dest_bootx64), "%s/BOOTX64.EFI", efi_boot_dir);
    if (copy_file(boot_info->bootx64_path, dest_bootx64) != 0) {
        fprintf(stderr, "ERROR: Failed to copy BOOTX64.EFI (disk full, permission denied, or I/O error)\n");
        return ISO_ERR_EXTRACT_FAILED;
    }

    /* Verify BOOTX64.EFI is signed — unsigned bootloaders will be rejected by Secure Boot */
    int signed_result = pki_is_signed(boot_info->bootx64_path);
    if (signed_result == 0) {
        const char *sb_warn = "WARNING: Windows BOOTX64.EFI is not Authenticode-signed. "
                              "This USB may fail to boot on systems with Secure Boot enabled.";
        fprintf(stderr, "%s\n", sb_warn);
        if (progress_cb) {
            progress_cb(8, sb_warn, NULL);
        }
    }

    if (progress_cb) {
        progress_cb(10, "Copied BOOTX64.EFI", NULL);
    }

    // Copy BOOTIA32.EFI if present
    if (boot_info->bootia32_path) {
        char dest_bootia32[PATH_MAX];
        snprintf(dest_bootia32, sizeof(dest_bootia32), "%s/BOOTIA32.EFI", efi_boot_dir);
        if (copy_file(boot_info->bootia32_path, dest_bootia32) != 0) {
            fprintf(stderr, "Warning: Failed to copy BOOTIA32.EFI (optional)\n");
        } else if (progress_cb) {
            progress_cb(15, "Copied BOOTIA32.EFI", NULL);
        }
    }

    // Copy BOOTAA64.EFI if present
    if (boot_info->bootaa64_path) {
        char dest_bootaa64[PATH_MAX];
        snprintf(dest_bootaa64, sizeof(dest_bootaa64), "%s/BOOTAA64.EFI", efi_boot_dir);
        if (copy_file(boot_info->bootaa64_path, dest_bootaa64) != 0) {
            fprintf(stderr, "Warning: Failed to copy BOOTAA64.EFI (optional)\n");
        } else if (progress_cb) {
            progress_cb(20, "Copied BOOTAA64.EFI", NULL);
        }
    }

    return ISO_OK;
}

/**
 * Setup BIOS boot environment
 */
static int setup_bios_boot(const char *mount_point,
                          const windows_boot_info_t *boot_info,
                          int filesystem_type,
                          winafi_progress_callback_t progress_cb) {
    (void)filesystem_type;  // BIOS boot works with any filesystem type
    if (!boot_info->bootmgr_path) {
        fprintf(stderr, "ERROR: BIOS boot requires bootmgr\n");
        return ISO_ERR_FILE_NOT_FOUND;
    }

    if (!boot_info->bcd_path) {
        fprintf(stderr, "ERROR: BIOS boot requires BCD file\n");
        return ISO_ERR_FILE_NOT_FOUND;
    }

    // Copy bootmgr to root of partition
    char dest_bootmgr[PATH_MAX];
    snprintf(dest_bootmgr, sizeof(dest_bootmgr), "%s/bootmgr", mount_point);
    struct stat _bm_st;
    if (stat(boot_info->bootmgr_path, &_bm_st) != 0) {
        fprintf(stderr, "ERROR: Source file not found: %s (errno: %d)\n",
                boot_info->bootmgr_path, errno);
        return ISO_ERR_FILE_NOT_FOUND;
    }
    if (copy_file(boot_info->bootmgr_path, dest_bootmgr) != 0) {
        fprintf(stderr, "ERROR: Failed to copy bootmgr (disk full, permission denied, or I/O error)\n");
        return ISO_ERR_EXTRACT_FAILED;
    }
    if (progress_cb) {
        progress_cb(25, "Copied bootmgr", NULL);
    }

    // Create Boot directory (note: Windows uses backslash, we use forward slash on Linux)
    char boot_dir[PATH_MAX];
    snprintf(boot_dir, sizeof(boot_dir), "%s/Boot", mount_point);
    if (create_dir_if_needed(boot_dir) != 0) {
        fprintf(stderr, "ERROR: Failed to create Boot directory\n");
        return ISO_ERR_EXTRACT_FAILED;
    }

    // Copy BCD file to Boot/BCD
    char dest_bcd[PATH_MAX];
    snprintf(dest_bcd, sizeof(dest_bcd), "%s/BCD", boot_dir);
    struct stat _bcd_st;
    if (stat(boot_info->bcd_path, &_bcd_st) != 0) {
        fprintf(stderr, "ERROR: Source file not found: %s (errno: %d)\n",
                boot_info->bcd_path, errno);
        return ISO_ERR_FILE_NOT_FOUND;
    }
    if (copy_file(boot_info->bcd_path, dest_bcd) != 0) {
        fprintf(stderr, "ERROR: Failed to copy BCD file (disk full, permission denied, or I/O error)\n");
        return ISO_ERR_EXTRACT_FAILED;
    }
    if (progress_cb) {
        progress_cb(30, "Copied BCD", NULL);
    }

    return ISO_OK;
}

/**
 * Setup Windows boot environment
 */
int setup_windows_boot(const char *mount_point,
                      const windows_boot_info_t *boot_info,
                      int filesystem_type,
                      winafi_progress_callback_t progress_cb) {
    // Parameter validation
    if (!mount_point) {
        return ISO_ERR_FILE_NOT_FOUND;
    }

    if (!boot_info) {
        return ISO_ERR_NO_BOOT_INFO;
    }

    // Require at least one boot file (UEFI or BIOS for successful write)
    if (!boot_info->bootx64_path && !boot_info->bootmgr_path) {
        fprintf(stderr, "ERROR: No boot files found (need BOOTX64.EFI or bootmgr)\n");
        return ISO_ERR_NO_BOOT_INFO;
    }

    // Verify mount point exists and is a directory
    struct stat st;
    if (stat(mount_point, &st) != 0) {
        fprintf(stderr, "ERROR: Mount point does not exist: %s\n", mount_point);
        return ISO_ERR_FILE_NOT_FOUND;
    }

    if (!S_ISDIR(st.st_mode)) {
        fprintf(stderr, "ERROR: Mount point is not a directory: %s\n", mount_point);
        return ISO_ERR_FILE_NOT_FOUND;
    }

    // Setup boot environment based on boot mode
    int ret = ISO_OK;

    switch (boot_info->boot_mode) {
        case ISO_BOOT_UEFI:
            ret = setup_uefi_boot(mount_point, boot_info, filesystem_type, progress_cb);
            break;

        case ISO_BOOT_BIOS:
            ret = setup_bios_boot(mount_point, boot_info, filesystem_type, progress_cb);
            break;

        case ISO_BOOT_HYBRID:
            // Setup both UEFI and BIOS
            // Try UEFI first
            if (boot_info->bootx64_path) {
                ret = setup_uefi_boot(mount_point, boot_info, filesystem_type, progress_cb);
                if (ret != ISO_OK) {
                    fprintf(stderr, "ERROR: UEFI boot setup failed\n");
                    return ret;
                }
            }

            // Then setup BIOS
            if (boot_info->bootmgr_path) {
                ret = setup_bios_boot(mount_point, boot_info, filesystem_type, progress_cb);
                if (ret != ISO_OK) {
                    fprintf(stderr, "ERROR: BIOS boot setup failed\n");
                    return ret;
                }
            }
            break;

        default:
            fprintf(stderr, "ERROR: Unknown boot mode: %d\n", boot_info->boot_mode);
            return ISO_ERR_NO_BOOT_INFO;
    }

    if (ret == ISO_OK && progress_cb) {
        progress_cb(100, "Windows boot setup complete", NULL);
    }

    return ret;
}
