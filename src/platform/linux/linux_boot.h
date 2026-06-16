#ifndef WINAFI_LINUX_BOOT_H
#define WINAFI_LINUX_BOOT_H

#include <limits.h>
#include <sys/param.h>
#include "iso_extract.h"

/**
 * Linux Boot Setup
 *
 * Sets up Linux boot environment for both UEFI and BIOS boot modes.
 * This module handles bootloader installation for Linux ISOs, including
 * GRUB2 for modern systems and Syslinux/ISOLINUX for compatibility.
 *
 * Bootloader types:
 * - GRUB2: Modern standard, works on both UEFI and BIOS
 *   - UEFI: Requires FAT32, uses /boot/grub/x86_64-efi/ modules
 *   - BIOS: Works with FAT32/NTFS/exFAT, uses /boot/grub/i386-pc/ modules
 * - Syslinux/ISOLINUX: Legacy bootloader for BIOS-only systems
 *   - Only supports BIOS boot mode
 *   - Files must be in same partition (cannot access elsewhere)
 * - Hybrid: Both GRUB2 and Syslinux present, GRUB2 primary
 *
 * References:
 * - GRUB2 manual: https://www.gnu.org/software/grub/manual/grub/
 * - Syslinux documentation: https://wiki.syslinux.org/wiki/index.php
 * - Linux boot standards: https://wiki.gentoo.org/wiki/Handbook:AMD64/Installation/Bootloaders
 */

/**
 * Boot loader types detected from Linux ISO
 */
typedef enum {
    LINUX_BOOT_UNKNOWN = 0,        // No bootloader detected
    LINUX_BOOT_GRUB2 = 1,          // GRUB2 only
    LINUX_BOOT_SYSLINUX = 2,       // Syslinux/ISOLINUX only
    LINUX_BOOT_GRUB2_SYSLINUX = 3, // Both present (hybrid)
} linux_boot_type_t;

/**
 * Information about Linux ISO boot requirements
 *
 * Populated by detect_linux_boot_type() after analyzing ISO structure.
 * Provides paths to bootloader files and configuration.
 */
typedef struct {
    linux_boot_type_t boot_type;                   // Detected bootloader type
    int is_uefi;                                   // 1 = UEFI capable, 0 = BIOS only
    char grub_cfg_path[PATH_MAX];                  // Full path to /boot/grub/grub.cfg
    char syslinux_cfg_path[PATH_MAX];              // Path to syslinux.cfg or isolinux.cfg
    char ldlinux_sys_path[PATH_MAX];               // Path to ldlinux.sys
    char vesamenu_c32_path[PATH_MAX];              // Path to vesamenu.c32
    char distro_name[256];                         // Distro name from /etc/os-release
    char shim_path[PATH_MAX];                      // Full path to shimx64.efi (empty if not found)
    char efi_bootloader_path[PATH_MAX];            // Full path to BOOTX64.EFI or grubx64.efi
    linux_sb_status_t sb_status;                   // Secure Boot support status
} linux_boot_info_t;

/**
 * Error codes for Linux boot operations
 */
#define LINUX_BOOT_OK                      0
#define LINUX_BOOT_ERR_NO_CONFIG           -1
#define LINUX_BOOT_ERR_INSTALL_FAILED      -2
#define LINUX_BOOT_ERR_FILESYSTEM_INCOMPATIBLE -3
#define LINUX_BOOT_ERR_SECURE_BOOT         -4
#define LINUX_BOOT_ERR_INVALID_PATH        -5

/**
 * Progress callback type for Linux boot setup
 * Signature: void callback(int percent, const char *message, void *user_data)
 */
#ifndef WINAFI_PROGRESS_CALLBACK_DEFINED
#define WINAFI_PROGRESS_CALLBACK_DEFINED
typedef void (*winafi_progress_callback_t)(int percent, const char *message, void *user_data);
#endif

/**
 * Detect Linux bootloader type from extracted ISO files
 *
 * Analyzes the extracted ISO directory structure to identify which bootloaders
 * are present (GRUB2, Syslinux, or both). Also detects if UEFI boot is possible.
 *
 * Detection logic:
 * - GRUB2: Looks for /boot/grub/grub.cfg
 * - Syslinux: Looks for /isolinux/isolinux.cfg or /syslinux/syslinux.cfg
 *   and required files ldlinux.sys and vesamenu.c32
 * - UEFI: Looks for /boot/efi/ or /boot/grub/x86_64-efi/ directory
 * - Distro: Parses /etc/os-release or fallback sources
 *
 * Parameters:
 *   iso_extract_path - Root directory where ISO has been extracted
 *   out_info        - Pointer to linux_boot_info_t to populate with results
 *
 * Returns:
 *   LINUX_BOOT_OK              - Success, out_info populated
 *   LINUX_BOOT_ERR_NO_CONFIG   - No bootloader configuration found
 *   LINUX_BOOT_ERR_INVALID_PATH - Invalid or unreadable extract path
 *
 * Note: Distro detection is informational and does not affect return code
 */
int detect_linux_boot_type(const char *iso_extract_path, linux_boot_info_t *out_info);

/**
 * Setup GRUB2 boot environment
 *
 * Configures GRUB2 bootloader on the mounted partition.
 * Supports both UEFI and BIOS boot modes with appropriate module installation.
 *
 * UEFI Boot (requires FAT32):
 * - Creates /boot/grub/x86_64-efi/ directory
 * - Copies grub.cfg and GRUB2 UEFI modules (*.mod files)
 *
 * BIOS Boot (works with FAT32/NTFS/exFAT):
 * - Creates /boot/grub/i386-pc/ directory
 * - Copies grub.cfg and GRUB2 BIOS modules (*.mod files)
 *
 * Parameters:
 *   mount_point     - Mount point of target partition
 *   boot_info       - Boot information from detect_linux_boot_type()
 *   filesystem_type - Target filesystem: FSTYPE_FAT32 (1), FSTYPE_NTFS (2), FSTYPE_EXFAT (3)
 *   is_uefi         - 1 for UEFI boot, 0 for BIOS boot
 *   progress_cb     - Progress callback (may be NULL)
 *
 * Returns:
 *   LINUX_BOOT_OK                  - Success
 *   LINUX_BOOT_ERR_INSTALL_FAILED  - Failed to copy files
 *   LINUX_BOOT_ERR_FILESYSTEM_INCOMPATIBLE - UEFI requires FAT32
 *   LINUX_BOOT_ERR_INVALID_PATH    - Invalid paths
 */
int setup_grub2_boot(const char *mount_point,
                     const linux_boot_info_t *boot_info,
                     int filesystem_type,
                     int is_uefi,
                     winafi_progress_callback_t progress_cb);

/**
 * Setup Syslinux boot environment
 *
 * Configures Syslinux/ISOLINUX bootloader on the mounted partition.
 * Syslinux only supports BIOS boot, not UEFI.
 *
 * Operations:
 * - Creates /syslinux/ or /isolinux/ directory as needed
 * - Copies syslinux.cfg or isolinux.cfg
 * - Copies ldlinux.sys, vesamenu.c32, and all other Syslinux modules
 *
 * Parameters:
 *   mount_point     - Mount point of target partition
 *   boot_info       - Boot information from detect_linux_boot_type()
 *   filesystem_type - Target filesystem: FSTYPE_FAT32 (1), FSTYPE_NTFS (2), FSTYPE_EXFAT (3)
 *   progress_cb     - Progress callback (may be NULL)
 *
 * Returns:
 *   LINUX_BOOT_OK                  - Success
 *   LINUX_BOOT_ERR_INSTALL_FAILED  - Failed to copy files
 *   LINUX_BOOT_ERR_FILESYSTEM_INCOMPATIBLE - Invalid for UEFI
 *   LINUX_BOOT_ERR_INVALID_PATH    - Invalid paths
 */
int setup_syslinux_boot(const char *mount_point,
                        const linux_boot_info_t *boot_info,
                        int filesystem_type,
                        winafi_progress_callback_t progress_cb);

/**
 * Setup Linux boot environment (main entry point)
 *
 * High-level interface that detects bootloader type and configures appropriate
 * bootloaders. Handles both GRUB2 and Syslinux, with fallback support.
 *
 * Hybrid Boot Logic:
 * - If both GRUB2 and Syslinux present: Try GRUB2 first, add Syslinux as fallback
 * - If GRUB2 only: Install GRUB2
 * - If Syslinux only: Install Syslinux
 * - If neither: Return error
 *
 * Secure Boot Handling:
 * - Detects UEFI Secure Boot
 * - Adds warning message if enabled and GRUB2 modules not signed
 * - Returns success anyway (warning, not blocking error)
 *
 * Parameters:
 *   mount_point      - Mount point of target partition
 *   iso_extract_path - Root directory where ISO has been extracted
 *   boot_info        - Boot information (if NULL, calls detect_linux_boot_type())
 *   filesystem_type  - Target filesystem: FSTYPE_FAT32 (1), FSTYPE_NTFS (2), FSTYPE_EXFAT (3)
 *   progress_cb      - Progress callback (may be NULL)
 *
 * Returns:
 *   LINUX_BOOT_OK              - Success, bootloaders configured
 *   LINUX_BOOT_ERR_NO_CONFIG   - No bootloader found
 *   LINUX_BOOT_ERR_INSTALL_FAILED - Bootloader installation failed
 *   LINUX_BOOT_ERR_INVALID_PATH - Invalid paths
 */
int setup_linux_boot(const char *mount_point,
                     const char *iso_extract_path,
                     const linux_boot_info_t *boot_info,
                     int filesystem_type,
                     winafi_progress_callback_t progress_cb);

#endif // WINAFI_LINUX_BOOT_H
