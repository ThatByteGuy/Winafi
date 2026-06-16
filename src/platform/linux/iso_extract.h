#ifndef WINAFI_ISO_EXTRACT_H
#define WINAFI_ISO_EXTRACT_H

#include <stdint.h>
#include "iso.h"  // Get iso_info_t definition

#ifdef __cplusplus
extern "C" {
#endif

/**
 * ISO Operating System Types
 */
typedef enum {
    ISO_OS_UNKNOWN = 0,
    ISO_OS_WINDOWS = 1,
    ISO_OS_LINUX = 2
} iso_os_type_t;

/**
 * ISO Boot Mode Types
 */
typedef enum {
    ISO_BOOT_BIOS = 0,
    ISO_BOOT_UEFI = 1,
    ISO_BOOT_HYBRID = 2
} iso_boot_mode_t;

/**
 * ISO Error Codes
 */
typedef enum {
    ISO_OK = 0,
    ISO_ERR_FILE_NOT_FOUND = -1,
    ISO_ERR_NOT_ISO = -2,
    ISO_ERR_ARCHIVE_ERROR = -3,
    ISO_ERR_EXTRACT_FAILED = -4,
    ISO_ERR_NO_BOOT_INFO = -5
} iso_error_t;

/**
 * Windows Version Types
 */
typedef enum {
    WINDOWS_UNKNOWN = 0,
    WINDOWS_7 = 1,
    WINDOWS_10 = 2,
    WINDOWS_11 = 3,
    WINDOWS_PE = 4,
    WINDOWS_SERVER = 5
} windows_version_t;

/**
 * Detect ISO structure and OS type
 *
 * Scans the ISO file to determine:
 * - Operating system type (Windows, Linux, or unknown)
 * - Boot mode (BIOS, UEFI, or hybrid)
 * - Whether bootable files are present
 * - Total ISO size
 *
 * Detection logic:
 * - Windows ISO: Looks for /bootmgr, /boot/bcd, /efi/boot/bootx64.efi
 * - Linux ISO: Looks for /boot/vmlinuz, /init, /boot/grub/grub.cfg, /isolinux/isolinux.cfg
 * - Boot mode: UEFI if has EFI files, BIOS if has ISOLINUX/SYSLINUX, HYBRID if both
 *
 * Parameters:
 *   iso_path   - Path to ISO file (must not be NULL)
 *   out_info   - Pointer to iso_info_t structure to populate (must not be NULL)
 *
 * Returns:
 *   ISO_OK                    - Success, out_info populated
 *   ISO_ERR_FILE_NOT_FOUND    - ISO file not found or invalid path
 *   ISO_ERR_NOT_ISO          - File is not a valid ISO
 *   ISO_ERR_ARCHIVE_ERROR    - libarchive error reading ISO
 *   ISO_ERR_EXTRACT_FAILED   - Failed to extract/scan ISO contents
 *   ISO_ERR_NO_BOOT_INFO     - ISO exists but has no boot information
 */
int iso_detect_os(const char *iso_path, iso_info_t *out_info);

/**
 * Linux Secure Boot status as detected from ISO contents
 */
typedef enum {
    LINUX_SB_UNKNOWN    = 0,  // Not a Linux ISO or EFI presence unclear
    LINUX_SB_SHIM       = 1,  // shimx64.efi found — distro uses signed shim chain (SB compatible)
    LINUX_SB_SIGNED     = 2,  // Signed BOOTX64.EFI found without separate shim
    LINUX_SB_UNSIGNED   = 3   // UEFI EFI boot files present but no signed shim detected
} linux_sb_status_t;

/**
 * Detect Linux Secure Boot capability from ISO contents (no extraction needed)
 *
 * Scans the ISO file listing for shimx64.efi (the MS-signed first-stage bootloader
 * shipped by major distros like Ubuntu, Fedora, Debian). Presence of shim strongly
 * indicates the ISO supports UEFI Secure Boot out of the box.
 *
 * Parameters:
 *   iso_path - Path to the ISO file
 *
 * Returns:
 *   LINUX_SB_SHIM      - shimx64.efi found anywhere in ISO (Secure Boot ready)
 *   LINUX_SB_UNSIGNED  - EFI boot files found but no shim detected
 *   LINUX_SB_UNKNOWN   - No EFI boot files found or not a readable ISO
 */
linux_sb_status_t iso_detect_linux_sb_status(const char *iso_path);

/**
 * List all files in ISO archive
 *
 * Extracts a complete list of all files and directories in the ISO,
 * including their full paths as they appear in the ISO structure.
 * File paths are allocated dynamically and must be freed with iso_free_file_list().
 *
 * Parameters:
 *   iso_path   - Path to ISO file (must not be NULL)
 *   out_files  - Pointer to char** to receive file list (must not be NULL)
 *   out_count  - Pointer to int to receive file count (must not be NULL)
 *
 * Returns:
 *   ISO_OK                    - Success, out_files and out_count populated
 *   ISO_ERR_FILE_NOT_FOUND    - ISO file not found or invalid path
 *   ISO_ERR_NOT_ISO          - File is not a valid ISO
 *   ISO_ERR_ARCHIVE_ERROR    - libarchive error reading ISO
 *   ISO_ERR_EXTRACT_FAILED   - Failed to extract file list
 */
int iso_list_files(const char *iso_path, char ***out_files, int *out_count);

/**
 * Free ISO file list memory
 *
 * Frees the memory allocated by iso_list_files() for the file list.
 * Safe to call with NULL pointer.
 *
 * Parameters:
 *   files  - Pointer to file list (may be NULL)
 *   count  - Number of files in the list
 */
void iso_free_file_list(char **files, int count);

/**
 * Progress information structure for ISO extraction
 *
 * Provides detailed progress information during extraction
 */
typedef struct {
    const char *file_path;       // Current file being extracted
    uint64_t bytes_extracted;    // Total bytes extracted so far
    uint64_t total_size;         // Total ISO size in bytes
    int percent;                 // Overall progress percentage (0-100)
    const char *message;         // Status message (e.g., "Extracting files")
} iso_progress_info_t;

/**
 * Progress callback function signature for ISO extraction
 *
 * Called during file extraction to report progress. Callback can request
 * cancellation by returning non-zero value.
 *
 * Parameters:
 *   progress - Pointer to progress information structure (never NULL)
 *   user_data - User-provided data (may be NULL)
 *
 * Returns:
 *   0 - Continue extraction
 *   non-zero - Request cancellation (extraction will stop and clean up)
 */
typedef int (*iso_progress_callback_t)(const iso_progress_info_t *progress, void *user_data);

/**
 * Windows Boot Information Structure
 * Contains details about Windows boot files and version detected in ISO
 */
typedef struct {
    char *bootmgr_path;      // Path to bootmgr file (BIOS boot)
    char *bcd_path;          // Path to BCD file (boot configuration)
    char *bootx64_path;      // Path to BOOTX64.EFI (UEFI boot)
    char *bootia32_path;     // Path to BOOTIA32.EFI (UEFI 32-bit)
    char *bootaa64_path;     // Path to BOOTAA64.EFI (UEFI ARM64)
    char *wim_path;          // Path to install.wim or boot.wim
    char *esd_path;          // Path to install.esd (Windows 11)
    int windows_version;     // WINDOWS_10, WINDOWS_11, WINDOWS_PE, etc.
    int is_enterprise;       // 1 if Enterprise edition detected
    int is_server;           // 1 if Server edition detected
    int boot_mode;           // ISO_BOOT_BIOS, ISO_BOOT_UEFI, or ISO_BOOT_HYBRID
} windows_boot_info_t;

/**
 * Detect Windows version and boot file locations in ISO
 *
 * Analyzes a Windows ISO to determine the version, edition, and boot file
 * locations. This information is used for proper boot setup later.
 *
 * Detection logic:
 * - Windows 11: Look for install.esd, newer architecture patterns
 * - Windows 10: Look for install.wim, older patterns
 * - Windows PE: Look for boot.wim without install.wim
 * - Windows Server: Check for server-specific markers
 * - Boot files: Find bootmgr, BCD, BOOTX64.EFI paths
 *
 * Parameters:
 *   iso_info   - Pointer to iso_info_t (must have os_type == ISO_OS_WINDOWS)
 *   out_boot_info - Pointer to windows_boot_info_t to populate
 *
 * Returns:
 *   ISO_OK                    - Success, out_boot_info populated with boot file paths
 *   ISO_ERR_FILE_NOT_FOUND    - iso_info or out_boot_info is NULL
 *   ISO_ERR_NO_BOOT_INFO     - iso_info is not a Windows ISO or critical boot files missing
 *   ISO_ERR_ARCHIVE_ERROR    - Error accessing ISO
 *
 * Note: Caller must free out_boot_info with free_windows_boot_info()
 */
int detect_windows_version(iso_info_t *iso_info, windows_boot_info_t *out_boot_info);

/**
 * Detailed Windows version detection using ISO file list
 *
 * Performs detailed analysis of Windows ISO using the complete file list.
 * Finds specific boot file paths and determines exact Windows version.
 *
 * Parameters:
 *   iso_path   - Path to ISO file for detailed analysis
 *   out_boot_info - Pointer to windows_boot_info_t to populate
 *
 * Returns:
 *   ISO_OK                    - Success, out_boot_info populated with boot file paths
 *   ISO_ERR_FILE_NOT_FOUND    - ISO file not found or parameters NULL
 *   ISO_ERR_NOT_ISO          - File is not a valid ISO
 *   ISO_ERR_NO_BOOT_INFO     - Not a Windows ISO or missing critical boot files
 *   ISO_ERR_ARCHIVE_ERROR    - Error accessing ISO
 *
 * Note: Caller must free out_boot_info with free_windows_boot_info()
 */
int detect_windows_version_detailed(const char *iso_path, windows_boot_info_t *out_boot_info);

/**
 * Free Windows boot information structure
 *
 * Frees all allocated memory in windows_boot_info_t, including all path strings.
 * Safe to call with NULL pointer.
 *
 * Parameters:
 *   boot_info - Pointer to windows_boot_info_t to free (may be NULL)
 */
void free_windows_boot_info(windows_boot_info_t *boot_info);

/**
 * Extract ISO file to a mounted device partition
 *
 * Extracts all files from ISO 9660 archive to the specified mount point,
 * preserving file structure, permissions, and handling special files.
 *
 * Supports:
 * - Regular files (any size, including >4GB)
 * - Symbolic links (created as symlinks, not resolved)
 * - Directories (created with proper permissions)
 * - File permissions preservation (chmod)
 * - Progress reporting via callback
 * - Graceful handling of special files (devices, sockets, pipes)
 *
 * Features:
 * - Progress callback invoked for each file extracted
 * - Callback can request cancellation (return non-zero)
 * - Proper cleanup on failure (partial files not left behind)
 * - Support for large files via libarchive 64-bit APIs
 * - Symlink creation with proper link targets
 * - Directory creation with mkdir -p style behavior
 * - File permission preservation where possible
 *
 * Parameters:
 *   iso_path      - Path to ISO 9660 file (must not be NULL)
 *   mount_point   - Mount point where ISO contents will be extracted (must not be NULL)
 *   progress_cb   - Progress callback function (may be NULL for no progress reporting)
 *   user_data     - Data passed to callback (may be NULL)
 *
 * Returns:
 *   ISO_OK                    - Success, all files extracted
 *   ISO_ERR_FILE_NOT_FOUND    - ISO or mount point not found
 *   ISO_ERR_NOT_ISO          - File is not a valid ISO
 *   ISO_ERR_ARCHIVE_ERROR    - libarchive error reading ISO
 *   ISO_ERR_EXTRACT_FAILED   - Error writing to mount point or other write error
 *
 * Progress Reporting:
 *   - Stage 4 of write operation (20-95% of overall progress)
 *   - File-by-file reporting during extraction
 *   - Callback receives iso_progress_info_t with bytes_extracted, total_size, and file path
 *   - Callback must return 0 to continue, non-zero to request cancellation
 *   - Cancellation cleans up partial files and returns ISO_ERR_EXTRACT_FAILED
 *
 * Example:
 *   iso_progress_callback_t cb = my_progress_function;
 *   int ret = iso_extract_to_mountpoint("/path/to/os.iso", "/mnt/usb",
 *                                      cb, NULL);
 *   if (ret != ISO_OK) {
 *       fprintf(stderr, "Extraction failed: %d\n", ret);
 *   }
 */
int iso_extract_to_mountpoint(const char *iso_path, const char *mount_point,
                             iso_progress_callback_t progress_cb, void *user_data);

#ifdef __cplusplus
}
#endif

#endif
