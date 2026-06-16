#ifndef WINAFI_WINDOWS_BOOT_H
#define WINAFI_WINDOWS_BOOT_H

#include "iso_extract.h"

/**
 * Progress callback type for Windows boot setup
 * Signature: void callback(int percent, const char *message, void *user_data)
 */
#ifndef WINAFI_PROGRESS_CALLBACK_DEFINED
#define WINAFI_PROGRESS_CALLBACK_DEFINED
typedef void (*winafi_progress_callback_t)(int percent, const char *message, void *user_data);
#endif

/**
 * Windows Boot Setup
 *
 * Sets up Windows boot environment for both UEFI and BIOS boot modes.
 * This module handles copying Windows boot files to their proper locations
 * on the formatted USB partition.
 *
 * References:
 * - Windows PE boot file structure: https://docs.microsoft.com/en-us/windows-hardware/manufacture/desktop/windows-pe-intro
 * - UEFI boot file layout: https://en.wikipedia.org/wiki/Unified_Extensible_Firmware_Interface
 * - BIOS boot configuration: https://en.wikipedia.org/wiki/Boot_Configuration_Data
 */

/**
 * Setup Windows boot environment for a formatted USB partition
 *
 * Configures the boot environment based on boot information from Windows ISO detection.
 * Supports both UEFI and BIOS boot modes, as well as hybrid boot.
 *
 * UEFI Boot Setup:
 * - Creates /EFI/BOOT/ directory structure on the partition
 * - Copies BOOTX64.EFI (x86-64 UEFI boot)
 * - Optionally copies BOOTIA32.EFI (32-bit UEFI boot)
 * - Optionally copies BOOTAA64.EFI (ARM64 UEFI boot)
 * - File names are uppercase (BOOTX64.EFI, not bootx64.efi)
 *
 * BIOS Boot Setup:
 * - Copies bootmgr to root of partition (no subdirectory)
 * - Creates \Boot\ directory structure
 * - Copies BCD file to Boot\BCD
 * - Copies supporting boot files from \Boot\ directory if present
 *
 * Hybrid Boot Setup:
 * - Performs both UEFI and BIOS setup for dual-boot capability
 *
 * Features:
 * - Preserves file permissions during copy
 * - Handles case-insensitive filesystems (FAT32, NTFS, exFAT)
 * - Progress reporting via callback
 * - Proper error handling with specific error codes
 * - Validation of boot files before setup
 *
 * Parameters:
 *   mount_point   - Mount point of the formatted partition (must be valid directory)
 *   boot_info     - Windows boot information from Windows ISO detection
 *                   Contains paths to boot files (bootmgr, BCD, BOOTX64.EFI, etc.)
 *   progress_cb   - Progress callback function (may be NULL for no reporting)
 *
 * Returns:
 *   ISO_OK                    - Success, boot environment configured
 *   ISO_ERR_FILE_NOT_FOUND    - Mount point not found or boot file doesn't exist
 *   ISO_ERR_EXTRACT_FAILED    - Failed to copy boot files
 *   ISO_ERR_NO_BOOT_INFO      - Invalid boot_info or no boot files specified
 *
 * Progress Reporting:
 * - Callback is invoked for each boot file copied
 * - Reports percentage of boot setup completion
 * - Reports current file being copied in message
 *
 * Boot File Validation:
 * - UEFI mode: Requires BOOTX64.EFI (BOOTIA32.EFI and BOOTAA64.EFI are optional)
 * - BIOS mode: Requires both bootmgr and BCD files
 * - Hybrid mode: Requires files for both boot modes
 * - Returns error if required files are missing
 *
 * Directory Creation:
 * - /EFI/BOOT/ is created with proper permissions if not present
 * - \Boot\ (converted to /Boot/) is created if not present
 * - Existing directories are not modified
 *
 * File Copying:
 * - Files are copied with permissions preserved where possible
 * - Symbolic links are NOT followed (regular files only)
 * - File names are converted to uppercase for EFI boot files
 * - Source file must exist before copying
 *
 * Filesystem Compatibility:
 * - Works with FAT32, NTFS, and exFAT (as returned from Tasks 4-6)
 * - Handles case-insensitive filesystems correctly
 * - Respects filesystem-specific naming constraints
 *
 * Example usage:
 *   windows_boot_info_t boot_info = { ... };  // From detect_windows_version()
 *   int ret = setup_windows_boot("/mnt/usb", &boot_info, progress_callback);
 *   if (ret != ISO_OK) {
 *       fprintf(stderr, "Boot setup failed: %d\n", ret);
 *   }
 */
int setup_windows_boot(const char *mount_point,
                      const windows_boot_info_t *boot_info,
                      int filesystem_type,
                      winafi_progress_callback_t progress_cb);

#endif
