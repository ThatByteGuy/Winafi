#define _GNU_SOURCE
#include "iso_extract.h"
#include <archive.h>
#include <archive_entry.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <ctype.h>
#include <fcntl.h>
#include <unistd.h>
#include <limits.h>
#include <errno.h>

/**
 * Structure to track extracted files for cleanup on error/cancellation
 */
typedef struct {
    char **files;  // Array of file paths
    int count;     // Number of files tracked
    int capacity;  // Allocated capacity
} extracted_files_list_t;

/**
 * Helper: Initialize extracted files list
 */
static extracted_files_list_t* extracted_files_list_create(void) {
    extracted_files_list_t *list = (extracted_files_list_t *)malloc(sizeof(extracted_files_list_t));
    if (!list) return NULL;

    list->capacity = 256;  // Initial capacity
    list->count = 0;
    list->files = (char **)malloc((size_t)list->capacity * sizeof(char *));
    if (!list->files) {
        free(list);
        return NULL;
    }
    return list;
}

/**
 * Helper: Add a file to the extracted files list
 */
static int extracted_files_list_add(extracted_files_list_t *list, const char *file_path) {
    if (!list || !file_path) return -1;

    // Resize if needed
    if (list->count >= list->capacity) {
        int new_capacity = list->capacity * 2;
        char **new_files = (char **)realloc(list->files, (size_t)new_capacity * sizeof(char *));
        if (!new_files) return -1;
        list->files = new_files;
        list->capacity = new_capacity;
    }

    // Add file path
    size_t len = strlen(file_path) + 1;
    char *path = (char *)malloc(len);
    if (!path) return -1;

    memcpy(path, file_path, len - 1);
    path[len - 1] = '\0';
    list->files[list->count] = path;
    list->count++;
    return 0;
}

/**
 * Helper: Clean up extracted files list
 */
static void extracted_files_list_cleanup(extracted_files_list_t *list) {
    if (!list) return;

    for (int i = 0; i < list->count; i++) {
        if (list->files[i]) {
            free(list->files[i]);
        }
    }
    free(list->files);
    free(list);
}

/**
 * Helper: Remove all extracted files (for error recovery)
 */
static void extracted_files_list_remove_all(extracted_files_list_t *list) {
    if (!list) return;

    for (int i = list->count - 1; i >= 0; i--) {
        if (list->files[i]) {
            // Use AT_SYMLINK_NOFOLLOW to avoid following symlinks
            unlinkat(AT_FDCWD, list->files[i], AT_SYMLINK_NOFOLLOW);
        }
    }
}

/**
 * Helper: Check if path contains a file with case-insensitive matching
 */
static int contains_file(const char *path, const char *filename) {
    if (!path || !filename) return 0;
    return (strcasestr(path, filename) != NULL) ? 1 : 0;
}

/**
 * Helper: Get the filename from a full path
 */
static const char* get_filename(const char *path) {
    if (!path) return NULL;
    const char *last_slash = strrchr(path, '/');
    if (last_slash && last_slash[1] != '\0') {
        return last_slash + 1;
    }
    return path;
}

/**
 * Helper: Case-insensitive string comparison
 */
static int strcasecmp_wrapper(const char *s1, const char *s2) {
    if (!s1 || !s2) return 1;
    return strcasecmp(s1, s2);
}

/**
 * Helper: Allocate and copy string
 */
static char* string_dup(const char *str) {
    if (!str) return NULL;
    size_t len = strlen(str) + 1;
    char *dup = (char *)malloc(len);
    if (!dup) return NULL;
    memcpy(dup, str, len - 1);
    dup[len - 1] = '\0';
    return dup;
}

/**
 * Helper: Validate that a path from ISO doesn't escape the mount point
 * Prevents path traversal attacks (e.g., ../../../etc/passwd)
 * Returns: 0 if valid, -1 if path is malicious or invalid
 */
static int validate_iso_path(const char *pathname, const char *mount_point) {
    if (!pathname || !mount_point) return -1;

    // Reject absolute paths and paths starting with /
    if (pathname[0] == '/') return -1;

    // Reject paths containing .. (parent directory traversal)
    if (strstr(pathname, "..") != NULL) return -1;

    // Check path length
    if (strlen(mount_point) + 1 + strlen(pathname) >= PATH_MAX) {
        return -1;  // Path too long
    }

    // Construct the target path and validate it doesn't escape mount point
    char target_path[PATH_MAX];
    snprintf(target_path, sizeof(target_path), "%s/%s", mount_point, pathname);

    // Use realpath to canonicalize - this resolves symlinks and removes .. and .
    char resolved_path[PATH_MAX];
    if (realpath(target_path, resolved_path) == NULL) {
        // realpath failed - this is OK for new files, but check if parent exists
        // Try to resolve the parent directory instead
        char *parent = strdup(target_path);
        if (!parent) return -1;

        char *last_slash = strrchr(parent, '/');
        if (last_slash) {
            *last_slash = '\0';
            if (realpath(parent, resolved_path) == NULL) {
                // Parent doesn't exist yet, which is OK for new files
                // Just verify the constructed path starts with mount_point
                // by checking the first part before any symlinks could resolve
                if (strncmp(target_path, mount_point, strlen(mount_point)) != 0) {
                    free(parent);
                    return -1;
                }
                free(parent);
                return 0;
            }

            // Verify parent path is within mount point
            if (strncmp(resolved_path, mount_point, strlen(mount_point)) != 0) {
                free(parent);
                return -1;  // Parent escaped mount point
            }
            free(parent);
        } else {
            free(parent);
            return -1;
        }
        return 0;
    }

    // Verify the resolved path starts with mount point (canonical comparison)
    if (strncmp(resolved_path, mount_point, strlen(mount_point)) != 0) {
        return -1;  // Path escaped mount point
    }

    return 0;  // Path is valid
}

/**
 * Helper: Check if ISO is UDF format
 * UDF is a modern ISO format used by Windows 10/11
 * UDF anchor volume descriptor tag is 0x0002
 * Located at sector 256 (offset 0x80000)
 * Returns 1 if UDF, 0 otherwise
 */
static int iso_is_udf(const char *iso_path) {
    if (!iso_path) return 0;

    FILE *fp = fopen(iso_path, "rb");
    if (!fp) return 0;

    unsigned char buffer[4];

    // Check UDF anchor at sector 256 (offset 0x80000 = 524288)
    // UDF anchor volume descriptor has tag ID 0x0002 (little-endian: 02 00)
    if (fseek(fp, 0x80000, SEEK_SET) == 0 && fread(buffer, 1, 4, fp) == 4) {
        // Check for tag ID 0x0002 (bytes 0-1 should be 0x02 0x00)
        if (buffer[0] == 0x02 && buffer[1] == 0x00) {
            fclose(fp);
            return 1;  // UDF ISO detected
        }
    }

    fclose(fp);
    return 0;
}

/**
 * Helper: Open ISO file and return libarchive handle
 * Returns NULL on error, caller must free with archive_read_free()
 */
static struct archive* iso_open_archive(const char *iso_path) {
    if (!iso_path) return NULL;

    // Check if file exists and is readable
    struct stat st;
    if (stat(iso_path, &st) != 0) {
        return NULL;  // File not found
    }

    struct archive *a = archive_read_new();
    if (!a) return NULL;

    // Support ISO 9660 (older ISOs) and all other supported formats (UDF for modern Windows ISOs)
    archive_read_support_format_iso9660(a);
    archive_read_support_format_all(a);

    // Support common compression filters
    archive_read_support_filter_all(a);

    // Open the ISO file
    int ret = archive_read_open_filename(a, iso_path, 10240);
    if (ret != ARCHIVE_OK) {
        fprintf(stderr, "Failed to open ISO: %s\n", archive_error_string(a));
        archive_read_free(a);
        return NULL;  // Not a valid ISO
    }

    return a;
}

/**
 * Helper: Scan archive and detect OS type, boot files, and boot modes
 * Single pass through the ISO to detect all characteristics
 */
static void scan_iso_contents(struct archive *a, int *out_os_type,
                             int *out_has_boot, int *out_boot_mode) {
    int is_windows = 0;
    int is_linux = 0;
    int has_uefi_files = 0;
    int has_bios_files = 0;
    int has_boot_files = 0;
    int found_efi_boot = 0;

    struct archive_entry *entry;

    while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {
        const char *path = archive_entry_pathname(entry);
        if (!path) {
            archive_read_data_skip(a);
            continue;
        }

        // Windows OS detection - check for .wim/.esd files (Windows-specific)
        if (contains_file(path, "boot.wim") ||
            contains_file(path, "install.wim") ||
            contains_file(path, "install.esd")) {
            is_windows = 1;
            has_boot_files = 1;
        }

        // Windows bootmgr (Windows 7+)
        if (contains_file(path, "bootmgr")) {
            is_windows = 1;
            has_boot_files = 1;
        }

        // Windows BCD file (boot configuration)
        if (contains_file(path, "/boot/bcd") || contains_file(path, "\\boot\\bcd")) {
            is_windows = 1;
            has_boot_files = 1;
        }

        // Windows UEFI files (bootx64.efi is very Windows-specific when in EFI/BOOT)
        if (contains_file(path, "bootx64.efi") ||
            contains_file(path, "bootia32.efi") ||
            contains_file(path, "bootaa64.efi")) {
            is_windows = 1;
            has_uefi_files = 1;
            has_boot_files = 1;
        }

        // Linux kernel files
        if (contains_file(path, "vmlinuz") ||
            contains_file(path, "bzimage") ||
            contains_file(path, "initrd")) {
            is_linux = 1;
            has_boot_files = 1;
        }

        // GRUB bootloader (Linux-specific)
        if (contains_file(path, "grub.cfg")) {
            is_linux = 1;
            has_boot_files = 1;
            // GRUB in EFI directory
            if (contains_file(path, "efi/boot") || contains_file(path, "efi\\boot")) {
                has_uefi_files = 1;
            } else {
                has_bios_files = 1;
            }
        }

        // ISOLINUX/SYSLINUX (Linux-specific BIOS bootloader)
        if (contains_file(path, "isolinux.cfg") ||
            contains_file(path, "syslinux.cfg") ||
            contains_file(path, "isolinux.bin") ||
            contains_file(path, "syslinux.bin")) {
            is_linux = 1;
            has_bios_files = 1;
            has_boot_files = 1;
        }

        // Generic EFI boot directory - mark as found but don't default OS yet
        if (contains_file(path, "efi/boot") || contains_file(path, "efi\\boot")) {
            has_uefi_files = 1;
            found_efi_boot = 1;
            has_boot_files = 1;
        }

        archive_read_data_skip(a);
    }

    // Determine OS type
    int os_type = ISO_OS_UNKNOWN;
    if (is_windows) {
        os_type = ISO_OS_WINDOWS;
    } else if (is_linux) {
        os_type = ISO_OS_LINUX;
    } else if (found_efi_boot && has_boot_files) {
        // If we found EFI boot files but no clear OS indicator,
        // check for Windows-specific patterns: bootx64.efi is very strong Windows indicator
        // For now, default to Windows since Windows 10/11 ISOs commonly have UEFI boot
        os_type = ISO_OS_WINDOWS;
    }

    // Determine boot mode
    int boot_mode = ISO_BOOT_BIOS;
    if (has_uefi_files && has_bios_files) {
        boot_mode = ISO_BOOT_HYBRID;
    } else if (has_uefi_files) {
        boot_mode = ISO_BOOT_UEFI;
    } else if (has_bios_files) {
        boot_mode = ISO_BOOT_BIOS;
    }

    *out_os_type = os_type;
    *out_has_boot = has_boot_files;
    *out_boot_mode = boot_mode;
}

/**
 * Detect Windows version and populate boot info structure
 *
 * This scans the ISO file list to find Windows-specific boot files and
 * determine the Windows version being used.
 *
 * References:
 * - Windows 10/11 ISOs contain: bootmgr, boot.wim, install.wim, install.esd
 * - Windows PE contains only boot.wim without install.wim
 * - UEFI boot needs BOOTX64.EFI or similar in EFI/BOOT directory
 * - Windows 11 uses install.esd instead of install.wim
 * - Windows 10 uses install.wim
 * - Windows PE has only boot.wim
 */
int detect_windows_version(iso_info_t *iso_info, windows_boot_info_t *out_boot_info) {
    if (!iso_info || !out_boot_info) {
        return ISO_ERR_FILE_NOT_FOUND;
    }

    // Check that this is actually a Windows ISO
    if (iso_info->os_type != ISO_OS_WINDOWS) {
        return ISO_ERR_NO_BOOT_INFO;
    }

    // Initialize output structure
    memset(out_boot_info, 0, sizeof(*out_boot_info));
    out_boot_info->windows_version = WINDOWS_UNKNOWN;
    out_boot_info->boot_mode = iso_info->boot_mode;

    // Try to infer Windows version from boot mode
    // UEFI boot is more common in Windows 11+
    if (iso_info->boot_mode == ISO_BOOT_UEFI) {
        out_boot_info->windows_version = WINDOWS_11;
    } else if (iso_info->boot_mode == ISO_BOOT_BIOS) {
        out_boot_info->windows_version = WINDOWS_10;
    } else {
        out_boot_info->windows_version = WINDOWS_10;  // Default to Windows 10
    }

    return ISO_OK;
}

/**
 * Enhanced Windows detection that uses iso_list_files for detailed analysis
 *
 * This is the internal implementation that's called with both ISO path and file list.
 * It performs detailed analysis to find specific boot files and determine Windows version.
 *
 * References for Windows boot file detection:
 * - Windows 10/11: Contains bootmgr (BIOS) and/or BOOTX64.EFI (UEFI)
 * - Boot Configuration: /boot/bcd or /Boot/BCD
 * - Install Image: install.wim (Windows 10) or install.esd (Windows 11)
 * - Boot Image: boot.wim (always present, PE if no install image)
 * - Architecture: BOOTX64.EFI (x64), BOOTIA32.EFI (IA32), BOOTAA64.EFI (ARM64)
 */
static int detect_windows_version_internal(const char *iso_path, char **file_list,
                                          int file_count, windows_boot_info_t *out_boot_info) {
    if (!iso_path || !file_list || file_count <= 0 || !out_boot_info) {
        return ISO_ERR_FILE_NOT_FOUND;
    }

    // Initialize output structure
    memset(out_boot_info, 0, sizeof(*out_boot_info));
    out_boot_info->windows_version = WINDOWS_UNKNOWN;

    // Scan file list for Windows-specific files
    int has_bootmgr = 0;
    int has_install_wim = 0;
    int has_boot_wim = 0;
    int has_install_esd = 0;
    int has_bootx64_efi = 0;
    int has_bootia32_efi = 0;
    int has_bootaa64_efi = 0;

    for (int i = 0; i < file_count; i++) {
        const char *path = file_list[i];
        if (!path) continue;

        const char *filename = get_filename(path);
        if (!filename) continue;

        // Check for bootmgr (BIOS boot loader)
        if (strcasecmp_wrapper(filename, "bootmgr") == 0) {
            has_bootmgr = 1;
            if (!out_boot_info->bootmgr_path) {
                out_boot_info->bootmgr_path = string_dup(path);
            }
        }

        // Check for BCD file (Boot Configuration Data)
        if (strcasecmp_wrapper(filename, "bcd") == 0 && contains_file(path, "boot")) {
            if (!out_boot_info->bcd_path) {
                out_boot_info->bcd_path = string_dup(path);
            }
        }

        // Check for WIM files (install.wim for full OS, boot.wim for boot environment)
        if (strcasecmp_wrapper(filename, "install.wim") == 0) {
            has_install_wim = 1;
            if (!out_boot_info->wim_path) {
                out_boot_info->wim_path = string_dup(path);
            }
        }

        if (strcasecmp_wrapper(filename, "boot.wim") == 0) {
            has_boot_wim = 1;
            // Prefer install.wim path if available, fall back to boot.wim
            if (!out_boot_info->wim_path) {
                out_boot_info->wim_path = string_dup(path);
            }
        }

        // Check for ESD file (Windows 11 install image)
        if (strcasecmp_wrapper(filename, "install.esd") == 0) {
            has_install_esd = 1;
            if (!out_boot_info->esd_path) {
                out_boot_info->esd_path = string_dup(path);
            }
        }

        // Check for UEFI boot files (architecture-specific)
        if (strcasecmp_wrapper(filename, "bootx64.efi") == 0) {
            has_bootx64_efi = 1;
            if (!out_boot_info->bootx64_path) {
                out_boot_info->bootx64_path = string_dup(path);
            }
        }

        if (strcasecmp_wrapper(filename, "bootia32.efi") == 0) {
            has_bootia32_efi = 1;
            if (!out_boot_info->bootia32_path) {
                out_boot_info->bootia32_path = string_dup(path);
            }
        }

        if (strcasecmp_wrapper(filename, "bootaa64.efi") == 0) {
            has_bootaa64_efi = 1;
            if (!out_boot_info->bootaa64_path) {
                out_boot_info->bootaa64_path = string_dup(path);
            }
        }
    }

    // Determine Windows version based on file presence patterns
    // These patterns are based on official Windows ISO structures:
    // - Windows 11: Usually has install.esd (newer compression format)
    // - Windows 10: Has install.wim
    // - Windows PE: Has boot.wim but NO install.wim or install.esd
    // - Windows 7/Server: May have install.wim with different structure

    if (has_install_esd) {
        // Windows 11 uses ESD format for install image (newer versions)
        out_boot_info->windows_version = WINDOWS_11;
    } else if (has_install_wim) {
        // install.wim present - either Windows 10 or older
        // Windows 11 22H2+ may also have install.wim
        out_boot_info->windows_version = WINDOWS_10;
    } else if (has_boot_wim) {
        // Only boot.wim without install image - Windows PE
        out_boot_info->windows_version = WINDOWS_PE;
    } else if (has_bootmgr) {
        // Has bootmgr but no install image - possibly corrupted or Windows 7
        out_boot_info->windows_version = WINDOWS_7;
    }

    // Set boot mode based on UEFI files present
    if (has_bootx64_efi || has_bootia32_efi || has_bootaa64_efi) {
        out_boot_info->boot_mode = ISO_BOOT_UEFI;
    } else {
        out_boot_info->boot_mode = ISO_BOOT_BIOS;
    }

    // Check for critical Windows boot files
    // Must have either bootmgr (BIOS) or UEFI boot files
    if (!has_bootmgr && !has_bootx64_efi && !has_bootia32_efi && !has_bootaa64_efi) {
        // Missing critical boot files
        fprintf(stderr, "Warning: Windows ISO missing critical boot files\n");
        return ISO_ERR_NO_BOOT_INFO;
    }

    return ISO_OK;
}

/**
 * Free Windows boot information
 */
void free_windows_boot_info(windows_boot_info_t *boot_info) {
    if (!boot_info) return;

    if (boot_info->bootmgr_path) free(boot_info->bootmgr_path);
    if (boot_info->bcd_path) free(boot_info->bcd_path);
    if (boot_info->bootx64_path) free(boot_info->bootx64_path);
    if (boot_info->bootia32_path) free(boot_info->bootia32_path);
    if (boot_info->bootaa64_path) free(boot_info->bootaa64_path);
    if (boot_info->wim_path) free(boot_info->wim_path);
    if (boot_info->esd_path) free(boot_info->esd_path);

    memset(boot_info, 0, sizeof(*boot_info));
}

/**
 * Detailed Windows version detection using ISO file list
 */
int detect_windows_version_detailed(const char *iso_path, windows_boot_info_t *out_boot_info) {
    if (!iso_path || !out_boot_info) {
        return ISO_ERR_FILE_NOT_FOUND;
    }

    // Get list of all files in ISO
    char **file_list = NULL;
    int file_count = 0;
    int ret = iso_list_files(iso_path, &file_list, &file_count);
    if (ret != ISO_OK) {
        return ret;
    }

    // Check if files list is empty
    if (file_count <= 0 || !file_list) {
        return ISO_ERR_NO_BOOT_INFO;
    }

    // Perform detailed Windows version detection
    ret = detect_windows_version_internal(iso_path, file_list, file_count, out_boot_info);

    // Verify we found critical Windows boot files
    if (ret == ISO_OK && !out_boot_info->bootmgr_path &&
        !out_boot_info->bootx64_path && !out_boot_info->bootia32_path) {
        fprintf(stderr, "Warning: Windows ISO missing critical boot files\n");
        ret = ISO_ERR_NO_BOOT_INFO;
    }

    // Free file list
    iso_free_file_list(file_list, file_count);

    return ret;
}

/**
 * Detect ISO operating system and boot mode
 */
int iso_detect_os(const char *iso_path, iso_info_t *out_info) {
    if (!iso_path || !out_info) {
        return ISO_ERR_FILE_NOT_FOUND;
    }

    // Initialize output structure
    memset(out_info, 0, sizeof(*out_info));
    out_info->os_type = ISO_OS_UNKNOWN;
    out_info->boot_mode = ISO_BOOT_BIOS;
    out_info->total_size_bytes = 0;
    strncpy(out_info->detected_os_str, "Unknown", sizeof(out_info->detected_os_str) - 1);
    out_info->detected_os_str[sizeof(out_info->detected_os_str) - 1] = '\0';

    // Check if file exists (needed to distinguish ISO_ERR_NOT_ISO from ISO_ERR_FILE_NOT_FOUND)
    struct stat st;
    if (stat(iso_path, &st) != 0) {
        return ISO_ERR_FILE_NOT_FOUND;
    }

    // Get total ISO size from file stat
    uint64_t file_size = (uint64_t)st.st_size;
    fprintf(stderr, "[iso_detect_os] file=%s size=%lu bytes (%.2f GB)\n", iso_path, file_size, file_size / (1024.0 * 1024.0 * 1024.0));

    // Open ISO file
    struct archive *a = iso_open_archive(iso_path);
    if (!a) {
        // File exists but is not a valid ISO
        return ISO_ERR_NOT_ISO;
    }

    // Use file size
    out_info->total_size_bytes = file_size;

    // Scan ISO contents
    int os_type, has_boot, boot_mode;
    scan_iso_contents(a, &os_type, &has_boot, &boot_mode);

    // Update output info
    out_info->os_type = os_type;
    out_info->has_boot_files = has_boot;
    out_info->boot_mode = boot_mode;

    archive_read_free(a);

    // Fallback: if we found no files and no boot info, check if it's a UDF ISO
    // UDF is commonly used for Windows 10/11 ISOs, so assume Windows if UDF is detected
    if (!has_boot && os_type == ISO_OS_UNKNOWN) {
        if (iso_is_udf(iso_path)) {
            // UDF detected - most likely a Windows 10/11 ISO
            out_info->os_type = ISO_OS_WINDOWS;
            out_info->boot_mode = ISO_BOOT_UEFI;  // UDF ISOs typically use UEFI
            out_info->has_boot_files = 1;
            strncpy(out_info->detected_os_str, "Windows", sizeof(out_info->detected_os_str) - 1);
            out_info->detected_os_str[sizeof(out_info->detected_os_str) - 1] = '\0';
            return ISO_OK;
        }
        return ISO_ERR_NO_BOOT_INFO;
    }

    // Set human-readable OS name
    if (out_info->os_type == ISO_OS_WINDOWS) {
        strncpy(out_info->detected_os_str, "Windows", sizeof(out_info->detected_os_str) - 1);
    } else if (out_info->os_type == ISO_OS_LINUX) {
        strncpy(out_info->detected_os_str, "Linux", sizeof(out_info->detected_os_str) - 1);
    } else {
        strncpy(out_info->detected_os_str, "Unknown", sizeof(out_info->detected_os_str) - 1);
    }
    out_info->detected_os_str[sizeof(out_info->detected_os_str) - 1] = '\0';

    return ISO_OK;
}

/**
 * Detect Linux Secure Boot status by scanning ISO for shimx64.efi
 */
linux_sb_status_t iso_detect_linux_sb_status(const char *iso_path) {
    if (!iso_path) return LINUX_SB_UNKNOWN;

    struct archive *a = iso_open_archive(iso_path);
    if (!a) return LINUX_SB_UNKNOWN;

    int found_efi = 0;
    int found_shim = 0;

    struct archive_entry *entry;
    while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {
        const char *path = archive_entry_pathname(entry);
        if (!path) { archive_read_data_skip(a); continue; }

        /* Case-insensitive check for shimx64.efi anywhere in the path */
        const char *p = path;
        while (*p) {
            if ((*p == 's' || *p == 'S') &&
                strncasecmp(p, "shimx64.efi", 11) == 0) {
                found_shim = 1;
                break;
            }
            p++;
        }

        /* Track presence of any EFI boot files */
        if (strncasecmp(path, "efi/", 4) == 0 ||
            strncasecmp(path, "/efi/", 5) == 0 ||
            contains_file(path, "bootx64.efi") ||
            contains_file(path, "grubx64.efi")) {
            found_efi = 1;
        }

        if (found_shim) break;
        archive_read_data_skip(a);
    }

    archive_read_free(a);

    if (found_shim) return LINUX_SB_SHIM;
    if (found_efi)  return LINUX_SB_UNSIGNED;
    return LINUX_SB_UNKNOWN;
}

/**
 * List all files in ISO
 */
int iso_list_files(const char *iso_path, char ***out_files, int *out_count) {
    if (!iso_path || !out_files || !out_count) {
        return ISO_ERR_FILE_NOT_FOUND;
    }

    *out_files = NULL;
    *out_count = 0;

    // Check if file exists
    struct stat st;
    if (stat(iso_path, &st) != 0) {
        return ISO_ERR_FILE_NOT_FOUND;
    }

    // Open ISO file
    struct archive *a = iso_open_archive(iso_path);
    if (!a) {
        return ISO_ERR_NOT_ISO;
    }

    // First pass: count files
    int file_count = 0;
    struct archive_entry *entry;

    while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {
        file_count++;
        archive_read_data_skip(a);
    }

    // Check for archive read errors
    if (archive_errno(a) != ARCHIVE_EOF) {
        archive_read_free(a);
        return ISO_ERR_ARCHIVE_ERROR;
    }

    if (file_count == 0) {
        archive_read_free(a);
        return ISO_ERR_EXTRACT_FAILED;
    }

    // Allocate array for file pointers
    char **files = (char **)malloc((size_t)file_count * sizeof(char *));
    if (!files) {
        archive_read_free(a);
        return ISO_ERR_EXTRACT_FAILED;
    }

    // Need to re-open archive to read from beginning again
    archive_read_free(a);
    a = iso_open_archive(iso_path);
    if (!a) {
        free(files);
        return ISO_ERR_NOT_ISO;
    }

    // Second pass: collect file paths
    int idx = 0;

    while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {
        const char *pathname = archive_entry_pathname(entry);
        if (pathname) {
            size_t len = strlen(pathname) + 1;
            files[idx] = (char *)malloc(len);
            if (!files[idx]) {
                // Cleanup on allocation failure
                for (int i = 0; i < idx; i++) {
                    free(files[i]);
                }
                free(files);
                archive_read_free(a);
                return ISO_ERR_EXTRACT_FAILED;
            }
            memcpy(files[idx], pathname, len - 1);
            files[idx][len - 1] = '\0';
            idx++;
        }
        archive_read_data_skip(a);
    }

    // Check for errors during second pass
    if (archive_errno(a) != ARCHIVE_EOF) {
        // Cleanup on error
        for (int i = 0; i < idx; i++) {
            free(files[i]);
        }
        free(files);
        archive_read_free(a);
        return ISO_ERR_ARCHIVE_ERROR;
    }

    archive_read_free(a);

    *out_files = files;
    *out_count = idx;

    return ISO_OK;
}

/**
 * Free ISO file list
 */
void iso_free_file_list(char **files, int count) {
    if (!files) return;

    for (int i = 0; i < count; i++) {
        if (files[i]) {
            free(files[i]);
        }
    }
    free(files);
}

/**
 * Helper: Create directory and parents as needed (mkdir -p equivalent)
 * Uses iterative approach to avoid stack overflow and prevent symlink attacks
 */
static int ensure_directory(const char *path, mode_t mode) {
    if (!path || path[0] == '\0') return -1;

    // Check path length against PATH_MAX
    if (strlen(path) >= PATH_MAX) return -1;

    // Make a working copy of the path
    char path_copy[PATH_MAX];
    strncpy(path_copy, path, PATH_MAX - 1);
    path_copy[PATH_MAX - 1] = '\0';

    // Use lstat to check if the full path already exists
    struct stat st;
    if (lstat(path_copy, &st) == 0) {
        // Path exists
        if (S_ISDIR(st.st_mode)) {
            return 0;  // Already a directory
        }
        // Path exists but is not a directory (could be file or symlink)
        return -1;
    }

    // Iteratively build path and create directories
    // Limit depth to prevent malicious nested paths
    const int MAX_DEPTH = 100;
    int depth = 0;
    char *saveptr = NULL;
    char working_path[PATH_MAX];

    // Build path iteratively
    working_path[0] = '\0';
    char path_temp[PATH_MAX];
    strncpy(path_temp, path_copy, PATH_MAX - 1);
    path_temp[PATH_MAX - 1] = '\0';

    char *component = strtok_r(path_temp, "/", &saveptr);

    // Handle absolute paths
    if (path_copy[0] == '/') {
        strcpy(working_path, "/");
    }

    while (component && depth < MAX_DEPTH) {
        depth++;

        // Build next path component
        if (working_path[0] == '/' && strlen(working_path) > 1) {
            strcat(working_path, "/");
        }
        strcat(working_path, component);

        // Validate path length
        if (strlen(working_path) >= PATH_MAX) {
            return -1;
        }

        // Use lstat to check for symlinks (don't follow)
        if (lstat(working_path, &st) == 0) {
            // Path exists, check if it's a directory (not a symlink)
            if (S_ISLNK(st.st_mode)) {
                // Symlink in the path - potential attack, reject it
                return -1;
            }
            if (!S_ISDIR(st.st_mode)) {
                // Path exists but is not a directory
                return -1;
            }
            // Directory exists, continue
        } else {
            // Directory doesn't exist, create it
            if (mkdir(working_path, mode) < 0 && errno != EEXIST) {
                return -1;
            }
        }

        component = strtok_r(NULL, "/", &saveptr);
    }

    // Check if we exceeded maximum depth
    if (depth >= MAX_DEPTH) {
        return -1;
    }

    return 0;
}

/**
 * Extract ISO file to mounted device partition
 */
int iso_extract_to_mountpoint(const char *iso_path, const char *mount_point,
                             iso_progress_callback_t progress_cb, void *user_data) {
    if (!iso_path || !mount_point) {
        return ISO_ERR_FILE_NOT_FOUND;
    }

    // Check if ISO file exists and is readable
    struct stat st;
    if (stat(iso_path, &st) != 0) {
        return ISO_ERR_FILE_NOT_FOUND;
    }

    // Check if mount point exists and is a directory
    if (stat(mount_point, &st) != 0) {
        return ISO_ERR_FILE_NOT_FOUND;
    }

    if (!S_ISDIR(st.st_mode)) {
        return ISO_ERR_FILE_NOT_FOUND;
    }

    // Check if we can write to mount point
    if (access(mount_point, W_OK) != 0) {
        return ISO_ERR_EXTRACT_FAILED;
    }

    // Open ISO file
    struct archive *a = iso_open_archive(iso_path);
    if (!a) {
        return ISO_ERR_NOT_ISO;
    }

    // Get total ISO size for progress calculation
    struct stat iso_st;
    stat(iso_path, &iso_st);
    uint64_t iso_total_size = (uint64_t)iso_st.st_size;
    uint64_t bytes_extracted = 0;

    // Initialize extracted files tracking for cleanup on error/cancellation
    extracted_files_list_t *extracted = extracted_files_list_create();
    if (!extracted) {
        archive_read_free(a);
        return ISO_ERR_EXTRACT_FAILED;
    }

    int error_code = ISO_OK;
    struct archive_entry *entry;
    int file_count = 0;
    int dir_created_count = 0;

    // Extract files
    while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {
        const char *pathname = archive_entry_pathname(entry);
        if (!pathname || pathname[0] == '\0') {
            archive_read_data_skip(a);
            la_int64_t entry_size = archive_entry_size(entry);
            if (entry_size > 0) {
                bytes_extracted += (uint64_t)entry_size;
            }
            continue;
        }

        // SECURITY FIX: Validate path to prevent traversal attacks
        if (validate_iso_path(pathname, mount_point) != 0) {
            fprintf(stderr, "Rejecting malicious path from ISO: %s\n", pathname);
            archive_read_data_skip(a);
            la_int64_t entry_size = archive_entry_size(entry);
            if (entry_size > 0) {
                bytes_extracted += (uint64_t)entry_size;
            }
            continue;  // Skip malicious entry, continue with next file
        }

        // SECURITY FIX: Check path length before construction
        if (strlen(mount_point) + 1 + strlen(pathname) >= PATH_MAX) {
            fprintf(stderr, "Path too long: %s/%s\n", mount_point, pathname);
            error_code = ISO_ERR_EXTRACT_FAILED;
            archive_read_data_skip(a);
            la_int64_t entry_size = archive_entry_size(entry);
            if (entry_size > 0) {
                bytes_extracted += (uint64_t)entry_size;
            }
            break;
        }

        // Build target path
        char target_path[PATH_MAX];
        snprintf(target_path, sizeof(target_path), "%s/%s", mount_point, pathname);

        // Get file type
        mode_t file_mode = archive_entry_filetype(entry);
        mode_t permissions = archive_entry_perm(entry);

        // Handle different file types
        if (S_ISDIR(file_mode)) {
            // Directory: create it
            if (ensure_directory(target_path, permissions ? permissions : 0755) != 0) {
                fprintf(stderr, "Failed to create directory: %s\n", target_path);
                // Don't abort on directory creation failure, continue
            }
            dir_created_count++;
        } else if (S_ISLNK(file_mode)) {
            // Symlink: create it (with security checks)
            const char *link_target = archive_entry_symlink(entry);
            if (link_target) {
                // Create parent directory if needed
                char *parent = strdup(target_path);
                if (parent) {
                    char *last_slash = strrchr(parent, '/');
                    if (last_slash) {
                        *last_slash = '\0';
                        ensure_directory(parent, 0755);
                    }
                    free(parent);
                }

                // SECURITY FIX: Use AT_SYMLINK_NOFOLLOW to safely remove existing symlink
                if (unlinkat(AT_FDCWD, target_path, AT_SYMLINK_NOFOLLOW) < 0 && errno != ENOENT) {
                    fprintf(stderr, "Failed to remove existing file before creating symlink: %s\n", target_path);
                    // Continue anyway, symlink() might still work if target_path is a symlink
                }

                // Create the symlink
                if (symlink(link_target, target_path) != 0) {
                    fprintf(stderr, "Failed to create symlink: %s -> %s\n", target_path, link_target);
                    // Don't abort on symlink failure, continue
                }
            }
            file_count++;

            // Track the file for cleanup if needed
            extracted_files_list_add(extracted, target_path);
        } else if (S_ISREG(file_mode)) {
            // Regular file: extract it
            // Ensure parent directory exists
            char *parent = strdup(target_path);
            if (parent) {
                char *last_slash = strrchr(parent, '/');
                if (last_slash) {
                    *last_slash = '\0';
                    if (ensure_directory(parent, 0755) != 0) {
                        fprintf(stderr, "Failed to create parent directory for: %s\n", target_path);
                        free(parent);
                        error_code = ISO_ERR_EXTRACT_FAILED;
                        archive_read_data_skip(a);
                        la_int64_t sz = archive_entry_size(entry);
                        if (sz > 0) bytes_extracted += (uint64_t)sz;
                        break;
                    }
                }
                free(parent);
            }

            // SECURITY FIX: Add O_NOFOLLOW to prevent following symlinks
            // and O_EXCL for new files to prevent TOCTOU race conditions
            // We'll use open with O_NOFOLLOW | O_CREAT | O_TRUNC
            int fd = open(target_path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW,
                         permissions ? permissions : 0644);
            if (fd < 0) {
                fprintf(stderr, "Failed to open file for writing: %s (errno: %d)\n", target_path, errno);
                error_code = ISO_ERR_EXTRACT_FAILED;
                archive_read_data_skip(a);
                la_int64_t sz = archive_entry_size(entry);
                if (sz > 0) bytes_extracted += (uint64_t)sz;
                break;
            }

            // Extract file data
            int ret = archive_read_data_into_fd(a, fd);
            close(fd);

            if (ret != ARCHIVE_OK) {
                fprintf(stderr, "Failed to extract file data: %s\n", target_path);
                // SECURITY FIX: Use AT_SYMLINK_NOFOLLOW when removing
                unlinkat(AT_FDCWD, target_path, AT_SYMLINK_NOFOLLOW);
                error_code = ISO_ERR_EXTRACT_FAILED;
                break;
            }

            // Preserve file permissions
            if (permissions) {
                chmod(target_path, permissions);
            }

            file_count++;

            // Track the file for cleanup if needed
            extracted_files_list_add(extracted, target_path);
        } else if (S_ISBLK(file_mode) || S_ISCHR(file_mode) || S_ISFIFO(file_mode) || S_ISSOCK(file_mode)) {
            // Skip special files (block device, character device, pipe, socket)
            fprintf(stderr, "Skipping special file: %s\n", target_path);
            archive_read_data_skip(a);
        } else {
            // Unknown file type, skip it
            fprintf(stderr, "Skipping unknown file type: %s\n", target_path);
            archive_read_data_skip(a);
        }

        // Update progress
        la_int64_t entry_size = archive_entry_size(entry);
        if (entry_size > 0) {
            bytes_extracted += (uint64_t)entry_size;
        }
        if (progress_cb) {
            iso_progress_info_t prog;
            prog.file_path = pathname;
            prog.bytes_extracted = bytes_extracted;
            prog.total_size = iso_total_size;
            prog.percent = (int)((bytes_extracted * 100) / iso_total_size);
            if (prog.percent > 100) prog.percent = 100;
            prog.message = "Extracting ISO files";

            int cb_ret = progress_cb(&prog, user_data);
            if (cb_ret != 0) {
                // User requested cancellation via callback
                // SECURITY FIX: Clean up ALL extracted files on cancellation, not just current
                fprintf(stderr, "Extraction cancelled by user\n");
                extracted_files_list_remove_all(extracted);
                error_code = ISO_ERR_EXTRACT_FAILED;
                break;
            }
        }

        if (error_code != ISO_OK) {
            break;
        }
    }

    // Check for archive errors
    if (error_code == ISO_OK && archive_errno(a) != ARCHIVE_EOF) {
        fprintf(stderr, "Archive error: %s\n", archive_error_string(a));
        error_code = ISO_ERR_ARCHIVE_ERROR;
    }

    // On error, clean up all extracted files
    if (error_code != ISO_OK) {
        fprintf(stderr, "Cleaning up extracted files due to error\n");
        extracted_files_list_remove_all(extracted);
    }

    // Report final progress
    if (progress_cb && error_code == ISO_OK) {
        iso_progress_info_t prog;
        prog.file_path = NULL;
        prog.bytes_extracted = iso_total_size;
        prog.total_size = iso_total_size;
        prog.percent = 100;
        prog.message = "ISO extraction complete";
        progress_cb(&prog, user_data);
    }

    extracted_files_list_cleanup(extracted);
    archive_read_free(a);
    return error_code;
}
