#include "winafi.h"  // Public API header - include first to avoid conflicts
#include "session.h"
#include "device.h"
#include "iso.h"
#include "iso_extract.h"
#include "partition.h"
#include "fs_ops.h"
#include "mount_ops.h"
#include "bootloader.h"
#include "windows_boot.h"
#include "linux_boot.h"
#include "wue.h"
#include "core/error.h"
#include "core/progress.h"
#include "core/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>

/**
 * Internal session structure
 * Opaque to callers - defined only in this file
 */
struct winafi_session {
    // State tracking
    int current_state;
    char error_code[16];      // "E-30-D" format
    char error_message[512];  // Full error message

    // Loaded data
    iso_info_t iso_info;
    winafi_device_t *devices;
    int device_count;
    char iso_path[512];
    char selected_device[64]; // /dev/sdX

    // Format options storage
    winafi_partition_scheme_t partition_scheme;    // default WINAFI_PARTITION_GPT
    winafi_target_system_t target_system;          // default WINAFI_TARGET_UEFI
    winafi_filesystem_t filesystem;                // default WINAFI_FS_NTFS
    uint32_t cluster_size;                        // default 0 (auto)
    char volume_label[256];                       // default empty string
    int quick_format;                             // default 1 (enabled)
    int bad_blocks_enabled;                       // default 0 (disabled)
    int bad_blocks_passes;                        // default 1
    winafi_image_option_t image_option;            // default WINAFI_IMAGE_STANDARD
    uint64_t bad_blocks_count;                    // default 0 (set after check)

    // Windows unattended customization (Feature 1/4/5). WUE_* flag bits from wue.h.
    int unattend_flags;                           // default 0 (no customization)
    char unattend_username[256];                  // local account name (WUE_SET_USER)

    // Module contexts
    winafi_device_context_t *device_ctx;
    winafi_partition_ctx_t *partition_ctx;

    // Callbacks
    progress_context_t progress_ctx;

    // Temporary data for execution
    mount_context_t mount_ctx;
};

/**
 * Set error state
 *
 * Stores error code and message in session, changes state to ERROR.
 *
 * @session: Session context
 * @error_code: String error code (e.g., "E-01-C")
 * @error_msg: Error message (may be NULL)
 */
static void session_set_error(winafi_session_t *session,
                              const char *error_code,
                              const char *error_msg) {
    if (!session) return;

    if (error_code) {
        strncpy(session->error_code, error_code, sizeof(session->error_code) - 1);
        session->error_code[sizeof(session->error_code) - 1] = '\0';
    } else {
        session->error_code[0] = '\0';
    }

    if (error_msg) {
        strncpy(session->error_message, error_msg, sizeof(session->error_message) - 1);
        session->error_message[sizeof(session->error_message) - 1] = '\0';
    } else {
        session->error_message[0] = '\0';
    }

    session->current_state = WINAFI_SESSION_ERROR;
}

/**
 * Check if we have root privileges
 *
 * Return: 1 if root, 0 otherwise
 */
static int check_root(void) {
    return geteuid() == 0 ? 1 : 0;
}

/**
 * Check if device exists and is a block device
 *
 * @devnode: Device path (e.g., "/dev/sdb")
 *
 * Return: 1 if valid block device, 0 otherwise
 */
static int device_is_block_device(const char *devnode) {
    struct stat st;
    if (stat(devnode, &st) != 0) {
        return 0;
    }
    return S_ISBLK(st.st_mode) ? 1 : 0;
}

/**
 * Check if ISO file exists
 *
 * @iso_path: Path to ISO file
 *
 * Return: 1 if exists, 0 otherwise
 */
static int iso_file_exists(const char *iso_path) {
    struct stat st;
    return (stat(iso_path, &st) == 0 && S_ISREG(st.st_mode)) ? 1 : 0;
}

/**
 * winafi_session_create - Create a new session
 */
winafi_session_t *winafi_session_create(void) {
    winafi_session_t *session = malloc(sizeof(*session));
    if (!session) {
        log_error("%s", "Failed to allocate session");
        return NULL;
    }

    memset(session, 0, sizeof(*session));
    session->current_state = WINAFI_SESSION_CREATED;

    // Initialize format option fields with defaults
    session->partition_scheme = WINAFI_PARTITION_GPT;      // default GPT
    session->target_system = WINAFI_TARGET_UEFI;           // default UEFI
    session->filesystem = WINAFI_FS_NTFS;                  // default NTFS
    session->cluster_size = 0;                            // 0 = auto
    session->volume_label[0] = '\0';                      // empty string
    session->quick_format = 1;                            // enabled by default
    session->bad_blocks_enabled = 0;                      // disabled by default
    session->bad_blocks_passes = 1;                       // 1 pass by default
    session->image_option = WINAFI_IMAGE_STANDARD;         // standard install
    session->bad_blocks_count = 0;                        // no bad blocks found yet

    // Initialize device context
    session->device_ctx = device_init();
    if (!session->device_ctx) {
        log_error("%s", "Failed to initialize device context");
        free(session);
        return NULL;
    }

    // Initialize partition context
    session->partition_ctx = partition_init();
    if (!session->partition_ctx) {
        log_error("%s", "Failed to initialize partition context");
        device_cleanup(session->device_ctx);
        free(session);
        return NULL;
    }

    return session;
}

/**
 * winafi_session_destroy - Destroy a session and free resources
 */
void winafi_session_destroy(winafi_session_t *session) {
    if (!session) return;

    // Free device list
    if (session->devices) {
        device_free_list(session->devices);
        session->devices = NULL;
    }

    // Cleanup partition context
    if (session->partition_ctx) {
        partition_cleanup(session->partition_ctx);
        session->partition_ctx = NULL;
    }

    // Cleanup device context
    if (session->device_ctx) {
        device_cleanup(session->device_ctx);
        session->device_ctx = NULL;
    }

    free(session);
}

/**
 * winafi_enumerate_devices - Enumerate USB devices
 */
int winafi_enumerate_devices(winafi_session_t *session,
                            winafi_device_t **devices,
                            int *device_count) {
    if (!session || !devices || !device_count) {
        log_error("%s", "Invalid parameters to enumerate_devices");
        if (session) {
            session_set_error(session, "E-51-A", "Invalid function parameters");
        }
        return -1;
    }

    // Free previous device list if any
    if (session->devices) {
        device_free_list(session->devices);
        session->devices = NULL;
        session->device_count = 0;
    }

    // Enumerate devices
    if (device_enumerate(session->device_ctx, &session->devices, &session->device_count) != 0) {
        log_error("%s", "Failed to enumerate devices");
        session_set_error(session, "E-00-A", "USB device enumeration failed");
        return -1;
    }

    *devices = session->devices;
    *device_count = session->device_count;
    session->current_state = WINAFI_SESSION_DEVICES_ENUMERATED;

    log_info("Enumerated %d devices", session->device_count);
    return 0;
}

/**
 * winafi_session_load_iso - Load and validate Windows ISO
 */
int winafi_session_load_iso(winafi_session_t *session, const char *iso_path) {
    iso_info_t iso_info;

    if (!session || !iso_path) {
        log_error("%s", "Invalid parameters to load_iso");
        if (session) {
            session_set_error(session, "E-51-A", "Invalid function parameters");
        }
        return -1;
    }

    // Can only load ISO from CREATED state
    if (session->current_state != WINAFI_SESSION_CREATED &&
        session->current_state != WINAFI_SESSION_DEVICES_ENUMERATED) {
        log_error("Cannot load ISO in state %d", session->current_state);
        session_set_error(session, "E-51-B", "Invalid operation sequence");
        return -1;
    }

    // Check if file exists
    if (!iso_file_exists(iso_path)) {
        log_error("ISO file not found: %s", iso_path);
        session_set_error(session, "E-10-A", "Windows ISO file not found");
        return -1;
    }

    // Detect ISO and extract metadata (replaces iso_validate_windows)
    int ret = iso_detect_os(iso_path, &iso_info);
    if (ret != ISO_OK) {
        log_error("Failed to detect ISO OS type: %s (ret=%d)", iso_path, ret);
        session_set_error(session, "E-10-C", "Not a valid ISO 9660 image");
        return -1;
    }

    // For now, only support Windows and Linux ISOs
    if (iso_info.os_type != ISO_OS_WINDOWS && iso_info.os_type != ISO_OS_LINUX) {
        log_error("Unsupported ISO type: %d", iso_info.os_type);
        session_set_error(session, "E-10-C", "Not a valid Windows or Linux ISO");
        return -1;
    }

    // TODO (Phase 2 Refinement): Improve Windows ISO detection for ESD format
    // For now: Skip install.wim check for ESD format ISOs that validated successfully
    // In future: Properly detect ESD format and handle accordingly
    // Temporarily disabled for hardware testing: iso_has_windows_install_wim(iso_path)

    // Store ISO info
    memcpy(&session->iso_info, &iso_info, sizeof(iso_info));
    strncpy(session->iso_path, iso_path, sizeof(session->iso_path) - 1);
    session->iso_path[sizeof(session->iso_path) - 1] = '\0';

    session->current_state = WINAFI_SESSION_ISO_LOADED;
    log_info("Loaded ISO: %s (size: %lu bytes)", iso_path, iso_info.total_size_bytes);
    return 0;
}

/**
 * winafi_session_select_device - Select target device
 */
int winafi_session_select_device(winafi_session_t *session, const char *devnode) {
    if (!session || !devnode) {
        log_error("%s", "Invalid parameters to select_device");
        if (session) {
            session_set_error(session, "E-51-A", "Invalid function parameters");
        }
        return -1;
    }

    // Must have loaded ISO before selecting device
    if (session->current_state < WINAFI_SESSION_ISO_LOADED) {
        log_error("Cannot select device before loading ISO (state %d)", session->current_state);
        session_set_error(session, "E-51-B", "Invalid operation sequence");
        return -1;
    }

    // Validate device exists in enumerated list
    int found = 0;
    for (int i = 0; i < session->device_count; i++) {
        if (strcmp(session->devices[i].devnode, devnode) == 0) {
            found = 1;
            break;
        }
    }

    if (!found) {
        log_error("Device not found: %s", devnode);
        session_set_error(session, "E-00-A", "USB device not found");
        return -1;
    }

    // Basic validation
    if (device_validate(devnode) != 0) {
        log_error("Device validation failed: %s", devnode);
        session_set_error(session, "E-00-G", "Device is locked by another process");
        return -1;
    }

    strncpy(session->selected_device, devnode, sizeof(session->selected_device) - 1);
    session->selected_device[sizeof(session->selected_device) - 1] = '\0';
    session->current_state = WINAFI_SESSION_DEVICE_SELECTED;

    log_info("Selected device: %s", devnode);
    return 0;
}

/**
 * winafi_session_prepare - Prepare for execution
 */
int winafi_session_prepare(winafi_session_t *session) {
    if (!session) {
        log_error("%s", "NULL session in prepare");
        return -1;
    }

    // Must be in DEVICE_SELECTED state
    if (session->current_state != WINAFI_SESSION_DEVICE_SELECTED) {
        log_error("Cannot prepare in state %d", session->current_state);
        session_set_error(session, "E-51-B", "Invalid operation sequence");
        return -1;
    }

    progress_fire(&session->progress_ctx, 0, "Validating environment");

    // Check root privileges
    if (!check_root()) {
        log_error("%s", "Not running as root");
        session_set_error(session, "E-01-C", "Insufficient privileges");
        return -1;
    }

    // Check device is block device
    if (!device_is_block_device(session->selected_device)) {
        log_error("Not a block device: %s", session->selected_device);
        session_set_error(session, "E-01-A", "Cannot access USB device");
        return -1;
    }

    // Check ISO exists
    if (!iso_file_exists(session->iso_path)) {
        log_error("ISO file no longer exists: %s", session->iso_path);
        session_set_error(session, "E-10-A", "Windows ISO file not found");
        return -1;
    }

    // Find device in list to get capacity
    uint64_t device_capacity = 0;
    for (int i = 0; i < session->device_count; i++) {
        if (strcmp(session->devices[i].devnode, session->selected_device) == 0) {
            device_capacity = session->devices[i].capacity_bytes;
            break;
        }
    }

    if (device_capacity == 0) {
        log_error("%s", "Cannot determine device capacity");
        session_set_error(session, "E-00-D", "Cannot read device properties");
        return -1;
    }

    // Verify ISO file still exists and get actual current size
    struct stat st;
    if (stat(session->iso_path, &st) != 0) {
        log_error("ISO file no longer accessible: %s", session->iso_path);
        session_set_error(session, "E-10-A", "Windows ISO file not found");
        return -1;
    }

    uint64_t actual_iso_size = (uint64_t)st.st_size;

    // Log both cached and actual sizes for debugging
    log_info("ISO size check - cached: %lu bytes (%.2f GB), actual: %lu bytes (%.2f GB)",
             session->iso_info.total_size_bytes,
             session->iso_info.total_size_bytes / (1024.0 * 1024.0 * 1024.0),
             actual_iso_size,
             actual_iso_size / (1024.0 * 1024.0 * 1024.0));

    // Use actual file size for comparison, not cached value
    if (device_capacity < actual_iso_size) {
        log_error("Device too small: %lu < %lu (ISO size)", device_capacity, actual_iso_size);
        log_error("ISO: %.2f GB, Device: %.2f GB",
                  actual_iso_size / (1024.0 * 1024.0 * 1024.0),
                  device_capacity / (1024.0 * 1024.0 * 1024.0));
        session_set_error(session, "E-00-C", "USB capacity insufficient");
        return -1;
    }

    session->current_state = WINAFI_SESSION_PREPARED;
    log_info("%s", "Session prepared and validated");
    return 0;
}

/**
 * winafi_session_execute - Execute the write operation
 *
 * Synchronous operation that performs complete USB write sequence
 */
int winafi_session_execute(winafi_session_t *session) {
    if (!session) {
        log_error("%s", "NULL session in execute");
        return -1;
    }

    // Must be in PREPARED state
    if (session->current_state != WINAFI_SESSION_PREPARED) {
        log_error("Cannot execute in state %d", session->current_state);
        session_set_error(session, "E-51-B", "Invalid operation sequence");
        return -1;
    }

    session->current_state = WINAFI_SESSION_EXECUTING;

    // Step 1: Wipe device partition table (0%)
    progress_fire(&session->progress_ctx, 0, "Wiping device");
    log_info("Step 1: Wiping device %s", session->selected_device);
    // Device wipe happens as part of partition_wipe_and_create

    // Step 2: Calculate partition layout and build partition device paths
    uint64_t total_sectors = 0;
    for (int i = 0; i < session->device_count; i++) {
        if (strcmp(session->devices[i].devnode, session->selected_device) == 0) {
            total_sectors = session->devices[i].capacity_bytes / 512;
            break;
        }
    }

    if (total_sectors == 0) {
        log_error("%s", "Failed to determine device capacity");
        session_set_error(session, "E-00-D", "Cannot read device properties");
        goto error;
    }

    // Build partition device paths (e.g., /dev/sdb1, /dev/sdb2)
    char fat_device[128], ntfs_device[128];
    snprintf(fat_device, sizeof(fat_device), "%s1", session->selected_device);
    snprintf(ntfs_device, sizeof(ntfs_device), "%s2", session->selected_device);

    uint64_t boot_size_bytes = 100 * 1024 * 1024;  // 100MB FAT32 boot partition

    // Step 3: Create partitions (10-30%)
    progress_fire(&session->progress_ctx, 10, "Creating partitions");
    log_info("%s", "Step 2-4: Creating partition table and partitions");

    if (partition_wipe_and_create(session->selected_device, total_sectors, boot_size_bytes) != 0) {
        log_error("%s", "Failed to create partitions");
        session_set_error(session, "E-20-A", "Cannot create partition table");
        goto error;
    }

    progress_fire(&session->progress_ctx, 30, "Formatting partitions");

    // Step 4: Format FAT32 (40%)
    log_info("Step 5: Formatting FAT32 boot partition %s", fat_device);
    if (fs_format_fat32(fat_device, "BOOT") != 0) {
        log_error("%s", "Failed to format FAT32");
        session_set_error(session, "E-21-A", "Cannot format FAT32 partition");
        goto error;
    }

    progress_fire(&session->progress_ctx, 40, "Formatting FAT32");

    // Step 5: Format NTFS (50%)
    log_info("Step 6: Formatting NTFS data partition %s", ntfs_device);
    if (fs_format_ntfs(ntfs_device, "WINDOWS") != 0) {
        log_error("%s", "Failed to format NTFS");
        session_set_error(session, "E-21-B", "Cannot format NTFS partition");
        goto error;
    }

    progress_fire(&session->progress_ctx, 50, "Formatting NTFS");

    // Step 6: Create temporary mount directories (60%)
    log_info("%s", "Step 7: Creating temporary mount directories");
    if (mount_create_temp_dirs(&session->mount_ctx) != 0) {
        log_error("%s", "Failed to create temporary directories");
        session_set_error(session, "E-50-B", "Cannot create temporary directory");
        goto error;
    }

    progress_fire(&session->progress_ctx, 60, "Mounting partitions");

    // Step 7: Mount FAT32 (65%)
    log_info("Step 8: Mounting FAT32 partition %s", fat_device);
    if (mount_fat32(fat_device, session->mount_ctx.fat_mount) != 0) {
        log_error("%s", "Failed to mount FAT32");
        session_set_error(session, "E-22-A", "Cannot mount partition");
        goto error;
    }

    // Step 8: Mount NTFS
    log_info("Step 9: Mounting NTFS partition %s", ntfs_device);
    if (mount_ntfs(ntfs_device, session->mount_ctx.ntfs_mount) != 0) {
        log_error("%s", "Failed to mount NTFS");
        session_set_error(session, "E-22-A", "Cannot mount partition");
        goto error;
    }

    progress_fire(&session->progress_ctx, 65, "Extracting ISO");

    // Step 9: Extract ISO (20-95%)
    log_info("%s", "Step 10: Extracting ISO to NTFS partition");
    // Note: iso_extract_to_mountpoint will report progress from 20-95% via callback
    int ret = iso_extract_to_mountpoint(session->iso_path, session->mount_ctx.ntfs_mount,
                                        NULL, NULL);
    if (ret != ISO_OK) {
        log_error("Failed to extract ISO: %d", ret);
        session_set_error(session, "E-30-A", "Cannot copy ISO files to USB");
        goto error;
    }

    progress_fire(&session->progress_ctx, 90, "Installing bootloaders");

    // Step 10-11: OS-specific boot setup (95-98%)
    log_info("Step 11-12: OS-specific boot setup (detected OS: %s)", session->iso_info.detected_os_str);

    if (session->iso_info.os_type == ISO_OS_WINDOWS) {
        // Windows-specific boot setup
        log_info("%s", "Setting up Windows boot environment");

        // Apply Windows unattended customization (Feature 1/4/5): requirement
        // bypasses, offline/local-account options, etc. The autounattend.xml must
        // live at the boot-media ROOT so Setup's windowsPE pass reads it.
        if (session->unattend_flags != 0) {
            log_info("Injecting unattend customization (flags 0x%04x)", session->unattend_flags);
            char *auto_xml = wue_generate_xml(session->unattend_flags,
                                              session->unattend_username[0] ? session->unattend_username : NULL,
                                              WUE_ARCH_X86_64);
            if (auto_xml) {
                if (wue_inject_autounattend(auto_xml, session->mount_ctx.ntfs_mount) != 0) {
                    log_error("%s", "Failed to inject autounattend.xml");
                    free(auto_xml);
                    session_set_error(session, "E-40-D", "Cannot inject Windows customization");
                    goto error;
                }
                free(auto_xml);
                log_info("%s", "autounattend.xml injected at media root");
            }
        }

        // Handle Windows To Go if requested
        if (session->image_option == WINAFI_IMAGE_WINTOGO) {
            log_info("%s", "Injecting Windows To Go configuration");

            // Generate WUE XML with offline drives flag (prevents installation on internal drives)
            char *wue_xml = wue_generate_xml(WUE_OFFLINE_DRIVES, NULL, WUE_ARCH_X86_64);
            if (wue_xml) {
                if (wue_inject_xml(wue_xml, session->mount_ctx.ntfs_mount) != 0) {
                    log_error("%s", "Failed to inject Windows To Go WUE XML");
                    free(wue_xml);
                    session_set_error(session, "E-40-C", "Cannot inject Windows To Go configuration");
                    goto error;
                }
                free(wue_xml);
                log_info("%s", "Windows To Go configuration injected successfully");
            }
        }

        // Detect Windows boot files
        windows_boot_info_t windows_boot_info;
        memset(&windows_boot_info, 0, sizeof(windows_boot_info));

        int ret = detect_windows_version_detailed(session->iso_path, &windows_boot_info);
        if (ret != ISO_OK) {
            log_info("Windows boot detection failed: %d", ret);
            // Fall through to try generic setup
        } else {
            // Pass progress callback if registered with session
            ret = setup_windows_boot(session->mount_ctx.ntfs_mount, &windows_boot_info,
                                     session->filesystem, session->progress_ctx.callback);
            if (ret != ISO_OK) {
                log_error("Windows boot setup failed: %d", ret);
                session_set_error(session, "E-40-A", "Cannot setup Windows boot environment");
                free_windows_boot_info(&windows_boot_info);
                goto error;
            }
            free_windows_boot_info(&windows_boot_info);
        }

        // UEFI: the firmware boots the signed UEFI:NTFS loader from the FAT32 ESP,
        // which chainloads Windows' own signed bootmgfw.efi from the NTFS partition.
        // This is REQUIRED for UEFI boot of NTFS Windows media. Treat failure as fatal.
        log_info("%s", "Installing signed UEFI:NTFS loader to ESP");
        if (bootloader_setup_uefi_ntfs(session->mount_ctx.fat_mount) != 0) {
            log_error("%s", "Failed to install UEFI:NTFS loader to ESP");
            session_set_error(session, "E-41-A", "Cannot install UEFI boot loader to ESP");
            goto error;
        }
    } else if (session->iso_info.os_type == ISO_OS_LINUX) {
        // Linux-specific boot setup
        log_info("%s", "Setting up Linux boot environment");

        // Mount point is where ISO was extracted; detect bootloader type and setup
        int ret = setup_linux_boot(session->mount_ctx.ntfs_mount, session->mount_ctx.ntfs_mount,
                                   NULL, session->filesystem, session->progress_ctx.callback);
        if (ret != LINUX_BOOT_OK) {
            log_error("Linux boot setup failed: %d", ret);
            session_set_error(session, "E-40-B", "Cannot setup Linux boot environment");
            goto error;
        }
    } else {
        // Fallback to generic bootloader setup
        log_info("%s", "Unknown OS type, using generic bootloader setup");

        // Install GRUB BIOS
        if (bootloader_setup_grub_bios(session->selected_device) != 0) {
            log_error("%s", "Failed to install GRUB");
            session_set_error(session, "E-40-A", "Cannot install GRUB bootloader");
            goto error;
        }

        // Setup UEFI:NTFS
        if (bootloader_setup_uefi_ntfs(session->mount_ctx.fat_mount) != 0) {
            log_error("%s", "Failed to setup UEFI:NTFS");
            session_set_error(session, "E-41-B", "Cannot configure UEFI boot partition");
            // Don't treat UEFI setup failure as fatal - BIOS boot should work
        }
    }

    progress_fire(&session->progress_ctx, 98, "Syncing filesystem");

    // Step 12: Sync filesystem (99%)
    log_info("%s", "Step 13: Syncing filesystem");
    if (mount_sync() != 0) {
        log_error("%s", "Failed to sync filesystem");
        session_set_error(session, "E-22-C", "Cannot flush buffers to disk");
        goto error;
    }

    // Step 13: Unmount partitions (100%)
    log_info("%s", "Step 14: Unmounting partitions");
    if (unmount_and_cleanup(&session->mount_ctx) != 0) {
        log_error("%s", "Failed to unmount partitions");
        session_set_error(session, "E-22-B", "Cannot unmount safely");
        goto error;
    }

    progress_fire(&session->progress_ctx, 100, "Complete");

    session->current_state = WINAFI_SESSION_COMPLETED;
    log_info("%s", "Execution completed successfully");
    return 0;

error:
    session->current_state = WINAFI_SESSION_ERROR;
    // Attempt cleanup
    unmount_and_cleanup(&session->mount_ctx);
    log_error("Execution failed with error: %s", session->error_code);
    return -1;
}

/**
 * winafi_get_error_code - Get last error code
 */
const char *winafi_get_error_code(winafi_session_t *session) {
    if (!session || session->error_code[0] == '\0') {
        return NULL;
    }
    return session->error_code;
}

/**
 * winafi_get_error_message - Get last error message
 */
const char *winafi_get_error_message(winafi_session_t *session) {
    if (!session || session->error_code[0] == '\0') {
        return NULL;
    }

    // Look up error message using error_lookup
    const char *msg = error_lookup(session->error_code);
    if (msg) {
        // Return stored message for now; in production this would be formatted
        return msg;
    }

    // Fallback to stored message
    if (session->error_message[0] != '\0') {
        return session->error_message;
    }

    return NULL;
}

/**
 * winafi_set_progress_callback - Register progress callback
 */
void winafi_set_progress_callback(winafi_session_t *session,
                                 winafi_progress_callback_t callback,
                                 void *user_data) {
    if (!session) return;

    progress_set_callback(&session->progress_ctx, callback, user_data);
}

/**
 * winafi_get_detected_os - Get detected OS from loaded ISO
 */
const char *winafi_get_detected_os(winafi_session_t *session) {
    if (!session) {
        return NULL;
    }

    // Return the version field from iso_info
    if (session->iso_info.detected_os_str[0] != '\0') {
        return session->iso_info.detected_os_str;
    }

    return NULL;
}

/**
 * winafi_get_linux_sb_status - Get Linux Secure Boot status for loaded ISO
 */
linux_sb_status_t winafi_get_linux_sb_status(winafi_session_t *session) {
    if (!session) return LINUX_SB_UNKNOWN;
    if (session->iso_info.os_type != ISO_OS_LINUX) return LINUX_SB_UNKNOWN;
    if (session->iso_path[0] == '\0') return LINUX_SB_UNKNOWN;
    return iso_detect_linux_sb_status(session->iso_path);
}

/**
 * winafi_get_image_option - Get detected image option from loaded ISO
 */
winafi_image_option_t winafi_get_image_option(winafi_session_t *session) {
    if (!session) {
        return WINAFI_IMAGE_STANDARD;
    }

    // For now, always return standard installation
    // In the future, this could detect based on ISO contents
    // (e.g., presence of VHD-related files for VHD, etc.)
    return WINAFI_IMAGE_STANDARD;
}

/**
 * winafi_session_set_image_option - Set the Windows installation type/image option
 */
int winafi_session_set_image_option(winafi_session_t *session, winafi_image_option_t option) {
    if (!session) {
        log_error("%s", "NULL session in set_image_option");
        return -1;
    }

    // Validate image option value
    if (option != WINAFI_IMAGE_STANDARD && option != WINAFI_IMAGE_PORTABLE &&
        option != WINAFI_IMAGE_VHD && option != WINAFI_IMAGE_WINTOGO) {
        log_error("Invalid image option value: %d", option);
        return -1;
    }

    session->image_option = option;
    log_info("Setting image option: %d", option);
    return WINAFI_OK;
}

/**
 * winafi_session_get_image_option - Get the currently set Windows installation type/image option
 */
winafi_image_option_t winafi_session_get_image_option(winafi_session_t *session) {
    if (!session) {
        log_error("%s", "NULL session in get_image_option");
        return WINAFI_IMAGE_STANDARD;
    }

    return session->image_option;
}

/**
 * winafi_session_set_unattend - Configure Windows unattended customization.
 * flags: bitwise-OR of WUE_* constants (see wue.h), e.g. WUE_BYPASS_ALL,
 *        WUE_NO_ONLINE_ACCOUNT, WUE_SET_USER. Pass 0 to disable customization.
 * username: local account name (used when WUE_SET_USER is set); may be NULL.
 * Returns WINAFI_OK on success, -1 on NULL session.
 */
int winafi_session_set_unattend(winafi_session_t *session, int flags, const char *username) {
    if (!session) {
        log_error("%s", "NULL session in set_unattend");
        return -1;
    }
    session->unattend_flags = flags;
    if (username && username[0])
        snprintf(session->unattend_username, sizeof(session->unattend_username), "%s", username);
    else
        session->unattend_username[0] = '\0';
    log_info("Setting unattend flags: 0x%04x", flags);
    return WINAFI_OK;
}

/**
 * winafi_session_set_partition_scheme - Set partition scheme (MBR/GPT)
 */
int winafi_session_set_partition_scheme(winafi_session_t *session, winafi_partition_scheme_t scheme) {
    if (!session) {
        log_error("%s", "NULL session in set_partition_scheme");
        return -1;
    }

    // Validate partition scheme value
    if (scheme != WINAFI_PARTITION_MBR && scheme != WINAFI_PARTITION_GPT) {
        log_error("Invalid partition scheme value: %d", scheme);
        return -1;
    }

    session->partition_scheme = scheme;
    log_info("Setting partition scheme: %d", scheme);
    return WINAFI_OK;
}

/**
 * winafi_session_get_partition_scheme - Get partition scheme
 */
winafi_partition_scheme_t winafi_session_get_partition_scheme(winafi_session_t *session) {
    if (!session) {
        log_error("%s", "NULL session in get_partition_scheme");
        return WINAFI_PARTITION_GPT;
    }

    return session->partition_scheme;
}

/**
 * winafi_session_set_target_system - Set target system (BIOS/UEFI)
 */
int winafi_session_set_target_system(winafi_session_t *session, winafi_target_system_t target) {
    if (!session) {
        log_error("%s", "NULL session in set_target_system");
        return -1;
    }

    // Validate target system value
    if (target != WINAFI_TARGET_BIOS && target != WINAFI_TARGET_UEFI) {
        log_error("Invalid target system value: %d", target);
        return -1;
    }

    session->target_system = target;
    log_info("Setting target system: %d", target);
    return WINAFI_OK;
}

/**
 * winafi_session_get_target_system - Get target system
 */
winafi_target_system_t winafi_session_get_target_system(winafi_session_t *session) {
    if (!session) {
        log_error("%s", "NULL session in get_target_system");
        return WINAFI_TARGET_UEFI;
    }

    return session->target_system;
}

/**
 * winafi_session_set_filesystem - Set filesystem type
 */
int winafi_session_set_filesystem(winafi_session_t *session, winafi_filesystem_t fs) {
    if (!session) {
        log_error("%s", "NULL session in set_filesystem");
        return -1;
    }

    // Validate filesystem value
    if (fs != WINAFI_FS_FAT32 && fs != WINAFI_FS_NTFS && fs != WINAFI_FS_EXFAT) {
        log_error("Invalid filesystem value: %d", fs);
        return -1;
    }

    session->filesystem = fs;
    log_info("Setting filesystem: %d", fs);
    return WINAFI_OK;
}

/**
 * winafi_session_get_filesystem - Get filesystem type
 */
winafi_filesystem_t winafi_session_get_filesystem(winafi_session_t *session) {
    if (!session) {
        log_error("%s", "NULL session in get_filesystem");
        return WINAFI_FS_NTFS;
    }

    return session->filesystem;
}

/**
 * winafi_session_set_cluster_size - Set cluster size
 *
 * Valid cluster sizes: 512, 1024, 2048, 4096 bytes
 */
int winafi_session_set_cluster_size(winafi_session_t *session, uint32_t cluster_size) {
    if (!session) {
        log_error("%s", "NULL session in set_cluster_size");
        return -1;
    }

    // Validate cluster size - only 512, 1024, 2048, 4096 are valid
    if (cluster_size != 512 && cluster_size != 1024 &&
        cluster_size != 2048 && cluster_size != 4096) {
        log_error("Invalid cluster size value: %u", cluster_size);
        return -1;
    }

    session->cluster_size = cluster_size;
    log_info("Setting cluster size: %u", cluster_size);
    return WINAFI_OK;
}

/**
 * winafi_session_set_volume_label - Set volume label
 *
 * Max 32 characters (FAT32 limitation)
 */
int winafi_session_set_volume_label(winafi_session_t *session, const char *label) {
    if (!session) {
        log_error("%s", "NULL session in set_volume_label");
        return -1;
    }

    if (!label) {
        log_error("%s", "NULL label in set_volume_label");
        return -1;
    }

    // Check label length - max 32 characters for FAT32 compatibility
    if (strlen(label) > 32) {
        log_error("Volume label exceeds maximum 32 characters: %zu", strlen(label));
        return -1;
    }

    strncpy(session->volume_label, label, sizeof(session->volume_label) - 1);
    session->volume_label[sizeof(session->volume_label) - 1] = '\0';
    log_info("Setting volume label: %s", label);
    return WINAFI_OK;
}

/**
 * winafi_session_get_volume_label - Get volume label
 *
 * Returns pointer to volume label or empty string on error
 */
const char *winafi_session_get_volume_label(winafi_session_t *session) {
    if (!session) {
        log_error("%s", "NULL session in get_volume_label");
        return "";
    }

    return session->volume_label;
}

/**
 * winafi_session_set_quick_format - Set quick format flag
 *
 * @quick: 0 = disabled, 1 = enabled
 */
int winafi_session_set_quick_format(winafi_session_t *session, int quick) {
    if (!session) {
        log_error("%s", "NULL session in set_quick_format");
        return -1;
    }

    // Validate quick format flag (0 or 1)
    if (quick != 0 && quick != 1) {
        log_error("Invalid quick format value: %d", quick);
        return -1;
    }

    session->quick_format = quick;
    log_info("Setting quick format: %d", quick);
    return WINAFI_OK;
}

/**
 * winafi_session_set_bad_blocks_check - Set bad blocks check
 *
 * @enabled: 0 = disabled, 1 = enabled
 * @passes: Number of passes (1-10 when enabled, ignored when disabled)
 */
int winafi_session_set_bad_blocks_check(winafi_session_t *session, int enabled, int passes) {
    if (!session) {
        log_error("%s", "NULL session in set_bad_blocks_check");
        return -1;
    }

    // Validate enabled flag (0 or 1)
    if (enabled != 0 && enabled != 1) {
        log_error("Invalid bad blocks enabled value: %d", enabled);
        return -1;
    }

    // Validate passes - only check if enabled
    if (enabled == 1) {
        if (passes < 1 || passes > 10) {
            log_error("Invalid bad blocks passes value: %d (must be 1-10 when enabled)", passes);
            return -1;
        }
    }

    session->bad_blocks_enabled = enabled;
    session->bad_blocks_passes = passes;
    log_info("Setting bad blocks check: enabled=%d, passes=%d", enabled, passes);
    return WINAFI_OK;
}

/**
 * winafi_session_get_bad_blocks_enabled - Get bad blocks enabled flag
 *
 * Return: 0 if disabled, 1 if enabled, 0 on error
 */
int winafi_session_get_bad_blocks_enabled(winafi_session_t *session) {
    if (!session) {
        log_error("%s", "NULL session in get_bad_blocks_enabled");
        return 0;
    }
    return session->bad_blocks_enabled;
}

/**
 * winafi_session_get_bad_blocks_passes - Get bad blocks passes count
 *
 * Return: Number of passes (1-10), defaults to 1 on error
 */
int winafi_session_get_bad_blocks_passes(winafi_session_t *session) {
    if (!session) {
        log_error("%s", "NULL session in get_bad_blocks_passes");
        return 1;
    }
    return session->bad_blocks_passes;
}
