#define _GNU_SOURCE
#include "linux_boot.h"
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
#include <dirent.h>

// Filesystem type constants
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
        return 0;
    }

    if (errno == EEXIST) {
        // Directory already exists, check it's actually a directory
        struct stat st;
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
            return 0;
        }
        return -1;
    }

    // Parent doesn't exist, try to create parent first
    if (errno == ENOENT) {
        char *path_copy = strdup(path);
        if (!path_copy) return -1;

        char *parent = dirname(path_copy);
        if (parent && strcmp(parent, path) != 0) {
            ret = create_dir_if_needed(parent);
            free(path_copy);
            if (ret != 0) return ret;

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
 * Returns: 0 on success, negative on error
 */
static int copy_file(const char *src_path, const char *dest_path) {
    if (!src_path || !dest_path) {
        return -1;
    }

    // Open source file for reading
    int src_fd = open(src_path, O_RDONLY | O_NOFOLLOW);
    if (src_fd < 0) {
        return -1;
    }

    // Get source file permissions
    struct stat st;
    if (fstat(src_fd, &st) < 0) {
        close(src_fd);
        return -1;
    }

    // Open destination file for writing
    int dest_fd = open(dest_path, O_WRONLY | O_CREAT | O_TRUNC, st.st_mode & 0777);
    if (dest_fd < 0) {
        close(src_fd);
        return -1;
    }

    // Copy file contents
    char buffer[4096];
    ssize_t bytes_read;
    while ((bytes_read = read(src_fd, buffer, sizeof(buffer))) > 0) {
        if (write(dest_fd, buffer, (size_t)bytes_read) != bytes_read) {
            close(src_fd);
            close(dest_fd);
            return -1;
        }
    }

    close(src_fd);
    close(dest_fd);

    if (bytes_read < 0) {
        return -1;
    }

    return 0;
}

/**
 * Helper: Check if file/directory exists and is readable
 */
static int file_exists(const char *path) {
    return access(path, F_OK) == 0;
}

/**
 * Helper: Check if path is a directory
 */
static int is_directory(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) {
        return 0;
    }
    return S_ISDIR(st.st_mode);
}


/**
 * Helper: Read a single line from a file (max length)
 *
 * Returns: 0 on success, negative on error
 */
static int read_file_line(const char *path, char *out_line, size_t max_len) {
    if (!path || !out_line || max_len == 0) {
        return -1;
    }

    int fd = open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) {
        return -1;
    }

    char buffer[4096];
    ssize_t bytes = read(fd, buffer, sizeof(buffer) - 1);
    close(fd);

    if (bytes <= 0) {
        return -1;
    }

    buffer[bytes] = '\0';

    // Find first newline
    char *newline = strchr(buffer, '\n');
    if (newline) {
        *newline = '\0';
    }

    size_t copy_len = strnlen(buffer, max_len - 1);
    memcpy(out_line, buffer, copy_len);
    out_line[copy_len] = '\0';

    return 0;
}

/* Common distro-specific EFI subdirectory names for shim search */
static const char *s_distro_efi_dirs[] = {
    "ubuntu", "fedora", "debian", "opensuse", "centos", "rocky",
    "rhel", "almalinux", "arch", "manjaro", "mint", "kali",
    "elementary", "pop", "zorin", NULL
};

/**
 * Helper: Find shimx64.efi in common locations under iso_extract_path.
 * Populates shim_path if found. Also locates BOOTX64.EFI / grubx64.efi.
 * Returns LINUX_SB_SHIM if shim found and signed, LINUX_SB_UNSIGNED otherwise.
 */
static linux_sb_status_t detect_shim_and_efi(const char *iso_extract_path,
                                              char *shim_path, size_t shim_size,
                                              char *efi_bootloader_path, size_t efi_size) {
    if (!iso_extract_path) return LINUX_SB_UNKNOWN;

    char candidate[PATH_MAX];
    shim_path[0] = '\0';
    efi_bootloader_path[0] = '\0';

    /* Check /EFI/BOOT/shimx64.efi first */
    snprintf(candidate, sizeof(candidate), "%s/EFI/BOOT/shimx64.efi", iso_extract_path);
    if (file_exists(candidate))
        snprintf(shim_path, shim_size, "%s", candidate);

    /* Check distro-specific EFI dirs: /EFI/<distro>/shimx64.efi */
    if (shim_path[0] == '\0') {
        for (int i = 0; s_distro_efi_dirs[i] != NULL; i++) {
            snprintf(candidate, sizeof(candidate), "%s/EFI/%s/shimx64.efi",
                     iso_extract_path, s_distro_efi_dirs[i]);
            if (file_exists(candidate)) {
                snprintf(shim_path, shim_size, "%s", candidate);
                break;
            }
        }
    }

    /* Locate the primary EFI bootloader (BOOTX64.EFI or grubx64.efi) */
    snprintf(candidate, sizeof(candidate), "%s/EFI/BOOT/BOOTX64.EFI", iso_extract_path);
    if (file_exists(candidate))
        snprintf(efi_bootloader_path, efi_size, "%s", candidate);

    if (efi_bootloader_path[0] == '\0') {
        snprintf(candidate, sizeof(candidate), "%s/EFI/BOOT/grubx64.efi", iso_extract_path);
        if (file_exists(candidate))
            snprintf(efi_bootloader_path, efi_size, "%s", candidate);
    }

    if (shim_path[0] != '\0') {
        /* Shim found — check if it's actually signed */
        int signed_result = pki_is_signed(shim_path);
        if (signed_result == 1) return LINUX_SB_SHIM;
        /* Shim present but unsigned — still treat as shim chain */
        return LINUX_SB_SHIM;
    }

    /* No shim — check if BOOTX64.EFI is directly signed */
    if (efi_bootloader_path[0] != '\0') {
        int signed_result = pki_is_signed(efi_bootloader_path);
        if (signed_result == 1) return LINUX_SB_SIGNED;
    }

    if (efi_bootloader_path[0] != '\0') return LINUX_SB_UNSIGNED;
    return LINUX_SB_UNKNOWN;
}

/**
 * Helper: Parse /etc/os-release for PRETTY_NAME
 *
 * Returns: 0 on success, negative on error
 */
static int detect_distro_from_os_release(const char *mount_point, char *out_distro, size_t name_size) {
    if (!mount_point || !out_distro || name_size == 0) {
        return -1;
    }

    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/etc/os-release", mount_point);

    if (!file_exists(path)) {
        return -1;
    }

    // Read os-release and find PRETTY_NAME
    int fd = open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) {
        return -1;
    }

    char buffer[4096];
    ssize_t bytes = read(fd, buffer, sizeof(buffer) - 1);
    close(fd);

    if (bytes <= 0) {
        return -1;
    }

    buffer[bytes] = '\0';

    // Find PRETTY_NAME line (use thread-safe strtok_r)
    char *saveptr = NULL;
    char *line = strtok_r(buffer, "\n", &saveptr);
    while (line) {
        if (strncmp(line, "PRETTY_NAME=", 12) == 0) {
            const char *value = line + 12;
            // Remove quotes if present
            if (*value == '"') {
                value++;
            }
            strncpy(out_distro, value, name_size - 1);
            out_distro[name_size - 1] = '\0';

            // Remove trailing quote
            size_t len = strlen(out_distro);
            if (len > 0 && out_distro[len - 1] == '"') {
                out_distro[len - 1] = '\0';
            }

            return 0;
        }
        line = strtok_r(NULL, "\n", &saveptr);
    }

    return -1;
}

/**
 * Helper: Detect distro from various sources
 */
static int detect_distro(const char *mount_point, char *out_distro_name, size_t name_size) {
    if (!mount_point || !out_distro_name || name_size == 0) {
        return -1;
    }

    // Try /etc/os-release first
    if (detect_distro_from_os_release(mount_point, out_distro_name, name_size) == 0) {
        return 0;
    }

    // Fallback sources
    char path[PATH_MAX];

    // Try /etc/redhat-release
    snprintf(path, sizeof(path), "%s/etc/redhat-release", mount_point);
    if (read_file_line(path, out_distro_name, name_size) == 0) {
        return 0;
    }

    // Try /etc/debian_version
    snprintf(path, sizeof(path), "%s/etc/debian_version", mount_point);
    if (file_exists(path)) {
        strncpy(out_distro_name, "Debian", name_size - 1);
        out_distro_name[name_size - 1] = '\0';
        return 0;
    }

    // Could not detect
    return -1;
}

/**
 * Detect Linux bootloader type from extracted ISO files
 */
int detect_linux_boot_type(const char *iso_extract_path, linux_boot_info_t *out_info) {
    if (!iso_extract_path || !out_info) {
        return LINUX_BOOT_ERR_INVALID_PATH;
    }

    // Initialize output
    memset(out_info, 0, sizeof(linux_boot_info_t));
    out_info->boot_type = LINUX_BOOT_UNKNOWN;

    // Check if extract path exists
    if (!is_directory(iso_extract_path)) {
        return LINUX_BOOT_ERR_INVALID_PATH;
    }

    int grub2_found = 0;
    int syslinux_found = 0;
    int uefi_found = 0;

    char path[PATH_MAX];

    // Check for GRUB2 configuration
    snprintf(path, sizeof(path), "%s/boot/grub/grub.cfg", iso_extract_path);
    if (file_exists(path)) {
        grub2_found = 1;
        snprintf(out_info->grub_cfg_path, PATH_MAX, "%s", path);
    }

    // Check for Syslinux/ISOLINUX
    char syslinux_dir[PATH_MAX];
    syslinux_dir[0] = '\0';

    snprintf(path, sizeof(path), "%s/isolinux/isolinux.cfg", iso_extract_path);
    if (file_exists(path)) {
        syslinux_found = 1;
        snprintf(out_info->syslinux_cfg_path, PATH_MAX, "%s", path);
        snprintf(syslinux_dir, sizeof(syslinux_dir), "%s/isolinux", iso_extract_path);
    } else {
        snprintf(path, sizeof(path), "%s/syslinux/syslinux.cfg", iso_extract_path);
        if (file_exists(path)) {
            syslinux_found = 1;
            snprintf(out_info->syslinux_cfg_path, PATH_MAX, "%s", path);
            snprintf(syslinux_dir, sizeof(syslinux_dir), "%s/syslinux", iso_extract_path);
        }
    }

    // Check for required Syslinux files if configuration found
    if (syslinux_found) {
        snprintf(path, sizeof(path), "%s/ldlinux.sys", syslinux_dir);
        if (file_exists(path)) {
            snprintf(out_info->ldlinux_sys_path, PATH_MAX, "%s", path);
        } else {
            syslinux_found = 0;  // Required file missing
        }

        snprintf(path, sizeof(path), "%s/vesamenu.c32", syslinux_dir);
        if (file_exists(path)) {
            snprintf(out_info->vesamenu_c32_path, PATH_MAX, "%s", path);
        } else {
            syslinux_found = 0;  // Required file missing
        }
    }

    // Check for UEFI capability
    snprintf(path, sizeof(path), "%s/boot/efi", iso_extract_path);
    if (is_directory(path)) {
        uefi_found = 1;
    }

    snprintf(path, sizeof(path), "%s/boot/grub/x86_64-efi", iso_extract_path);
    if (is_directory(path)) {
        uefi_found = 1;
    }

    out_info->is_uefi = uefi_found ? 1 : 0;

    /* Detect shim and EFI bootloader for Secure Boot chain setup */
    out_info->sb_status = detect_shim_and_efi(iso_extract_path,
                                               out_info->shim_path, PATH_MAX,
                                               out_info->efi_bootloader_path, PATH_MAX);

    // Determine boot type based on what was found
    if (grub2_found && syslinux_found) {
        out_info->boot_type = LINUX_BOOT_GRUB2_SYSLINUX;
    } else if (grub2_found) {
        out_info->boot_type = LINUX_BOOT_GRUB2;
    } else if (syslinux_found) {
        out_info->boot_type = LINUX_BOOT_SYSLINUX;
    } else {
        return LINUX_BOOT_ERR_NO_CONFIG;
    }

    // Try to detect distro (informational, non-blocking)
    detect_distro(iso_extract_path, out_info->distro_name, sizeof(out_info->distro_name));

    return LINUX_BOOT_OK;
}

/**
 * Setup GRUB2 boot environment
 */
int setup_grub2_boot(const char *mount_point,
                     const linux_boot_info_t *boot_info,
                     int filesystem_type,
                     int is_uefi,
                     winafi_progress_callback_t progress_cb) {
    if (!mount_point || !boot_info) {
        return LINUX_BOOT_ERR_INVALID_PATH;
    }

    if (!is_directory(mount_point)) {
        return LINUX_BOOT_ERR_INVALID_PATH;
    }

    // UEFI boot requires FAT32
    if (is_uefi && filesystem_type != FSTYPE_FAT32) {
        return LINUX_BOOT_ERR_FILESYSTEM_INCOMPATIBLE;
    }

    /* UEFI Secure Boot: install shim chain if shimx64.efi is present.
     * The correct chain is: UEFI firmware -> BOOTX64.EFI (shim, MS-signed)
     *                       -> grubx64.efi (distro-signed) -> kernel.
     * Without shim, unsigned GRUB will be rejected by Secure Boot firmware. */
    if (is_uefi && boot_info->shim_path[0] != '\0') {
        if (progress_cb) {
            progress_cb(25, "Installing Secure Boot shim chain...", NULL);
        }

        char efi_boot_dir[PATH_MAX];
        snprintf(efi_boot_dir, sizeof(efi_boot_dir), "%s/EFI/BOOT", mount_point);
        if (create_dir_if_needed(efi_boot_dir) != 0) {
            return LINUX_BOOT_ERR_INSTALL_FAILED;
        }

        /* Copy shim as BOOTX64.EFI (the UEFI firmware entry point) */
        char dest_bootx64[PATH_MAX + 16];
        snprintf(dest_bootx64, sizeof(dest_bootx64), "%s/BOOTX64.EFI", efi_boot_dir);
        if (copy_file(boot_info->shim_path, dest_bootx64) != 0) {
            fprintf(stderr, "ERROR: Failed to copy shimx64.efi as BOOTX64.EFI\n");
            return LINUX_BOOT_ERR_INSTALL_FAILED;
        }

        /* Copy grubx64.efi alongside shim so shim can load it */
        if (boot_info->efi_bootloader_path[0] != '\0') {
            char dest_grub[PATH_MAX + 16];
            snprintf(dest_grub, sizeof(dest_grub), "%s/grubx64.efi", efi_boot_dir);
            if (copy_file(boot_info->efi_bootloader_path, dest_grub) != 0) {
                /* Non-fatal: GRUB may already be in place from ISO extraction */
                fprintf(stderr, "Warning: Could not copy grubx64.efi alongside shim\n");
            }
        }

        if (progress_cb) {
            progress_cb(45, "Secure Boot shim chain installed", NULL);
        }
    } else if (is_uefi) {
        /* No shim available — warn that Secure Boot may block boot */
        const char *warning_msg = "WARNING: No signed shim found. "
                                  "This USB may not boot on systems with Secure Boot enabled.";
        fprintf(stderr, "%s\n", warning_msg);
        if (progress_cb) {
            progress_cb(25, warning_msg, NULL);
        }
    }

    if (progress_cb) {
        progress_cb(50, "Setting up GRUB2 bootloader...", NULL);
    }

    char path[PATH_MAX];

    // Copy grub.cfg
    if (file_exists(boot_info->grub_cfg_path)) {
        // Create parent directory
        snprintf(path, sizeof(path), "%s/boot", mount_point);
        create_dir_if_needed(path);
        snprintf(path, sizeof(path), "%s/boot/grub", mount_point);
        create_dir_if_needed(path);

        snprintf(path, sizeof(path), "%s/boot/grub/grub.cfg", mount_point);
        if (copy_file(boot_info->grub_cfg_path, path) < 0) {
            return LINUX_BOOT_ERR_INSTALL_FAILED;
        }
    }

    if (progress_cb) {
        progress_cb(75, "Copying GRUB2 modules...", NULL);
    }

    // Create appropriate directory for GRUB2 modules
    const char *modules_dir = is_uefi ? "x86_64-efi" : "i386-pc";
    snprintf(path, sizeof(path), "%s/boot/grub/%s", mount_point, modules_dir);
    if (create_dir_if_needed(path) < 0) {
        return LINUX_BOOT_ERR_INSTALL_FAILED;
    }

    // Copy GRUB2 modules from source
    if (boot_info->grub_cfg_path[0] != '\0') {
        char src_modules_dir[PATH_MAX];
        char cfg_dir[PATH_MAX];
        strncpy(cfg_dir, boot_info->grub_cfg_path, sizeof(cfg_dir) - 1);
        cfg_dir[sizeof(cfg_dir) - 1] = '\0';

        // Get directory from path
        char *last_slash = strrchr(cfg_dir, '/');
        if (last_slash) {
            *last_slash = '\0';
        }

        // Construct modules directory path
        if (snprintf(src_modules_dir, sizeof(src_modules_dir), "%s/%s",
                     cfg_dir, modules_dir) < 0 ||
            strlen(cfg_dir) + 1 + strlen(modules_dir) >= sizeof(src_modules_dir)) {
            return LINUX_BOOT_ERR_INSTALL_FAILED;
        }

        // Try to copy modules if directory exists
        if (is_directory(src_modules_dir)) {
            DIR *dir = opendir(src_modules_dir);
            if (dir) {
                struct dirent *entry;
                while ((entry = readdir(dir)) != NULL) {
                    if (entry->d_type == DT_REG && strstr(entry->d_name, ".mod")) {
                        if (snprintf(path, sizeof(path), "%s/%s",
                                     src_modules_dir, entry->d_name) < 0 ||
                            strlen(src_modules_dir) + 1 + strlen(entry->d_name) >= sizeof(path)) {
                            closedir(dir);
                            return LINUX_BOOT_ERR_INSTALL_FAILED;
                        }
                        char dest_path[PATH_MAX + 32];
                        snprintf(dest_path, sizeof(dest_path), "%s/boot/grub/%s/%s",
                                 mount_point, modules_dir, entry->d_name);

                        // Check return value and handle error
                        if (copy_file(path, dest_path) < 0) {
                            fprintf(stderr, "ERROR: Failed to copy GRUB2 module: %s\n", entry->d_name);
                            closedir(dir);
                            return LINUX_BOOT_ERR_INSTALL_FAILED;
                        }
                    }
                }
                closedir(dir);
            }
        }
    }

    if (progress_cb) {
        progress_cb(90, "GRUB2 setup complete", NULL);
    }

    return LINUX_BOOT_OK;
}

/**
 * Setup Syslinux boot environment
 */
int setup_syslinux_boot(const char *mount_point,
                        const linux_boot_info_t *boot_info,
                        int filesystem_type,
                        winafi_progress_callback_t progress_cb) {
    (void)filesystem_type;
    if (!mount_point || !boot_info) {
        return LINUX_BOOT_ERR_INVALID_PATH;
    }

    if (!is_directory(mount_point)) {
        return LINUX_BOOT_ERR_INVALID_PATH;
    }

    // Syslinux does not support UEFI
    if (boot_info->is_uefi) {
        return LINUX_BOOT_ERR_FILESYSTEM_INCOMPATIBLE;
    }

    if (progress_cb) {
        progress_cb(50, "Setting up Syslinux bootloader...", NULL);
    }

    char path[PATH_MAX + 32];

    // Determine target directory (isolinux or syslinux)
    const char *target_dir = NULL;
    if (strstr(boot_info->syslinux_cfg_path, "isolinux")) {
        target_dir = "isolinux";
    } else {
        target_dir = "syslinux";
    }

    snprintf(path, sizeof(path), "%s/%s", mount_point, target_dir);
    if (create_dir_if_needed(path) < 0) {
        return LINUX_BOOT_ERR_INSTALL_FAILED;
    }

    // Copy syslinux.cfg or isolinux.cfg
    snprintf(path, sizeof(path), "%s/%s/syslinux.cfg", mount_point, target_dir);
    if (strstr(boot_info->syslinux_cfg_path, "isolinux")) {
        snprintf(path, sizeof(path), "%s/%s/isolinux.cfg", mount_point, target_dir);
    }

    if (file_exists(boot_info->syslinux_cfg_path)) {
        if (copy_file(boot_info->syslinux_cfg_path, path) < 0) {
            return LINUX_BOOT_ERR_INSTALL_FAILED;
        }
    }

    if (progress_cb) {
        progress_cb(75, "Copying Syslinux modules...", NULL);
    }

    // Copy ldlinux.sys and vesamenu.c32
    if (file_exists(boot_info->ldlinux_sys_path)) {
        snprintf(path, sizeof(path), "%s/%s/ldlinux.sys", mount_point, target_dir);
        if (copy_file(boot_info->ldlinux_sys_path, path) < 0) {
            return LINUX_BOOT_ERR_INSTALL_FAILED;
        }
    }

    if (file_exists(boot_info->vesamenu_c32_path)) {
        snprintf(path, sizeof(path), "%s/%s/vesamenu.c32", mount_point, target_dir);
        if (copy_file(boot_info->vesamenu_c32_path, path) < 0) {
            return LINUX_BOOT_ERR_INSTALL_FAILED;
        }
    }

    // Copy other Syslinux modules from source directory
    char src_dir[PATH_MAX];
    snprintf(src_dir, sizeof(src_dir), "%s", boot_info->ldlinux_sys_path);
    char *last_slash = strrchr(src_dir, '/');
    if (last_slash) {
        *last_slash = '\0';
    }

    if (is_directory(src_dir)) {
        DIR *dir = opendir(src_dir);
        if (dir) {
            struct dirent *entry;
            while ((entry = readdir(dir)) != NULL) {
                if (entry->d_type == DT_REG) {
                    const char *ext = strrchr(entry->d_name, '.');
                    if (ext && (strcmp(ext, ".c32") == 0 || strcmp(ext, ".menu") == 0)) {
                        if (snprintf(path, sizeof(path), "%s/%s", src_dir, entry->d_name) < 0 ||
                            strlen(src_dir) + 1 + strlen(entry->d_name) >= sizeof(path)) {
                            closedir(dir);
                            return LINUX_BOOT_ERR_INSTALL_FAILED;
                        }
                        char dest_path[PATH_MAX + 32];
                        snprintf(dest_path, sizeof(dest_path), "%s/%s/%s",
                                 mount_point, target_dir, entry->d_name);

                        // Check return value and handle error
                        if (copy_file(path, dest_path) < 0) {
                            fprintf(stderr, "ERROR: Failed to copy Syslinux module: %s\n", entry->d_name);
                            closedir(dir);
                            return LINUX_BOOT_ERR_INSTALL_FAILED;
                        }
                    }
                }
            }
            closedir(dir);
        }
    }

    if (progress_cb) {
        progress_cb(90, "Syslinux setup complete", NULL);
    }

    return LINUX_BOOT_OK;
}

/**
 * Setup Linux boot environment (main entry point)
 */
int setup_linux_boot(const char *mount_point,
                     const char *iso_extract_path,
                     const linux_boot_info_t *boot_info,
                     int filesystem_type,
                     winafi_progress_callback_t progress_cb) {
    if (!mount_point || !iso_extract_path) {
        return LINUX_BOOT_ERR_INVALID_PATH;
    }

    if (!is_directory(mount_point)) {
        return LINUX_BOOT_ERR_INVALID_PATH;
    }

    // If boot_info not provided, detect it
    linux_boot_info_t detected_info;
    const linux_boot_info_t *boot_info_to_use = boot_info;

    if (!boot_info) {
        int ret = detect_linux_boot_type(iso_extract_path, &detected_info);
        if (ret != LINUX_BOOT_OK) {
            return ret;
        }
        boot_info_to_use = &detected_info;
    }

    if (boot_info_to_use->boot_type == LINUX_BOOT_UNKNOWN) {
        return LINUX_BOOT_ERR_NO_CONFIG;
    }

    int ret = LINUX_BOOT_OK;

    // Determine if we need UEFI boot
    int is_uefi = boot_info_to_use->is_uefi;

    if (progress_cb) {
        progress_cb(20, "Detecting Linux bootloader...", NULL);
    }

    // Handle different boot types
    switch (boot_info_to_use->boot_type) {
        case LINUX_BOOT_GRUB2:
            if (progress_cb) {
                progress_cb(30, "Setting up GRUB2 bootloader", NULL);
            }
            ret = setup_grub2_boot(mount_point, boot_info_to_use, filesystem_type, is_uefi, progress_cb);
            break;

        case LINUX_BOOT_SYSLINUX:
            if (progress_cb) {
                progress_cb(30, "Setting up Syslinux bootloader", NULL);
            }
            ret = setup_syslinux_boot(mount_point, boot_info_to_use, filesystem_type, progress_cb);
            break;

        case LINUX_BOOT_GRUB2_SYSLINUX:
            if (progress_cb) {
                progress_cb(30, "Setting up GRUB2 and Syslinux (hybrid boot)", NULL);
            }
            // Try GRUB2 first
            ret = setup_grub2_boot(mount_point, boot_info_to_use, filesystem_type, is_uefi, progress_cb);
            if (ret != LINUX_BOOT_OK) {
                // Fall back to Syslinux if GRUB2 failed
                if (progress_cb) {
                    progress_cb(50, "GRUB2 failed, falling back to Syslinux", NULL);
                }
                ret = setup_syslinux_boot(mount_point, boot_info_to_use, filesystem_type, progress_cb);
            } else {
                // Also set up Syslinux as secondary bootloader
                int syslinux_ret = setup_syslinux_boot(mount_point, boot_info_to_use, filesystem_type, progress_cb);
                // Don't fail overall if secondary bootloader fails
                if (syslinux_ret != LINUX_BOOT_OK && progress_cb) {
                    progress_cb(80, "Syslinux secondary bootloader setup failed (GRUB2 is primary)", NULL);
                }
            }
            break;

        default:
            return LINUX_BOOT_ERR_NO_CONFIG;
    }

    if (progress_cb) {
        progress_cb(100, "Linux boot setup complete", NULL);
    }

    return ret;
}
