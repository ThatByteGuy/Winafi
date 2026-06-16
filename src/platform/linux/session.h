#ifndef WINAFI_SESSION_H
#define WINAFI_SESSION_H

#include <stdint.h>
#include "device.h"
#include "iso.h"
#include "iso_extract.h"

/**
 * Session state machine for Winafi Linux
 *
 * The session manages the complete lifecycle of creating a bootable USB drive
 * from a Windows ISO. It orchestrates all platform modules (device, ISO, partition,
 * filesystem, mount, bootloader) and maintains error state.
 *
 * State Diagram:
 *   CREATED -> load_iso -> DEVICES_ENUMERATED
 *           -> enumerate_devices
 *           -> select_device -> DEVICE_SELECTED
 *           -> prepare -> PREPARED
 *           -> execute -> COMPLETED (or ERROR)
 *
 * Invariants:
 * - Cannot select_device() without load_iso() first
 * - Cannot prepare() without select_device()
 * - Cannot execute() without prepare()
 * - execute() is synchronous and blocks until completion
 * - All operations are fail-fast: errors halt execution and set state to ERROR
 */

typedef enum {
    WINAFI_SESSION_CREATED = 0,        // Initial state after creation
    WINAFI_SESSION_ISO_LOADED = 1,     // ISO file validated and loaded
    WINAFI_SESSION_DEVICES_ENUMERATED = 2,  // Devices enumerated
    WINAFI_SESSION_DEVICE_SELECTED = 3,     // Target device selected
    WINAFI_SESSION_PREPARED = 4,            // Validation and checks passed
    WINAFI_SESSION_EXECUTING = 5,           // Currently executing write operation
    WINAFI_SESSION_COMPLETED = 6,           // Successfully completed
    WINAFI_SESSION_ERROR = 7                // Error occurred
} winafi_session_state_t;

/**
 * Opaque session type
 * Internal structure is defined in session.c
 * Memory is managed by winafi_session_create() and winafi_session_destroy()
 */
typedef struct winafi_session winafi_session_t;

/**
 * Progress callback typedef (also defined in winafi.h)
 */
typedef void (*winafi_progress_callback_t)(int percent, const char *message, void *user_data);

/**
 * Create a new session
 *
 * Initializes a new session in CREATED state with no ISO loaded, no devices
 * enumerated, and no device selected.
 *
 * Return: Allocated session on success, NULL on allocation failure
 *         Caller must eventually call winafi_session_destroy()
 */
winafi_session_t *winafi_session_create(void);

/**
 * Destroy a session
 *
 * Frees all allocated resources including device list, ISO context,
 * and any temporary directories. Safe to call with NULL.
 *
 * @session: Session to destroy (may be NULL)
 */
void winafi_session_destroy(winafi_session_t *session);

/**
 * Enumerate USB devices
 *
 * Discovers all USB block devices on the system.
 *
 * State: Can be called from CREATED or ISO_LOADED state
 * Transitions to DEVICES_ENUMERATED on success
 *
 * @session: Session context
 * @devices: Output pointer to allocated device array (caller must NOT free)
 *           Will be automatically freed on destroy or next enumerate
 * @device_count: Output number of devices
 *
 * Return: 0 on success, -1 on error (session state becomes ERROR)
 */
int winafi_enumerate_devices(winafi_session_t *session,
                            winafi_device_t **devices,
                            int *device_count);

/**
 * Load a Windows ISO file
 *
 * Validates that the file exists, is readable, and contains a valid
 * Windows install.wim. Extracts ISO metadata (version, size, file count).
 *
 * State: Can only be called from CREATED state
 * Transitions to ISO_LOADED on success
 *
 * @session: Session context
 * @iso_path: Absolute path to ISO file
 *
 * Return: 0 on success, -1 on error (session state becomes ERROR)
 */
int winafi_session_load_iso(winafi_session_t *session, const char *iso_path);

/**
 * Select target USB device
 *
 * Selects which USB device to write to. The device must have been
 * enumerated and must be valid (not system disk, not mounted, etc.).
 *
 * State: Can only be called after ISO_LOADED and DEVICES_ENUMERATED
 * Transitions to DEVICE_SELECTED on success
 *
 * @session: Session context
 * @devnode: Device node (e.g., "/dev/sdb")
 *
 * Return: 0 on success, -1 on error (session state becomes ERROR)
 */
int winafi_session_select_device(winafi_session_t *session, const char *devnode);

/**
 * Prepare for execution
 *
 * Performs pre-flight validation:
 * - Check root privileges (geteuid() == 0)
 * - Verify device exists and is a block device
 * - Verify ISO exists and is valid Windows ISO
 * - Check device capacity >= ISO size + 100MB overhead
 * - Confirm device is not system disk
 * - Confirm device is not mounted
 *
 * Progress: Fires callback at 0% "Validating environment"
 *
 * State: Can only be called from DEVICE_SELECTED
 * Transitions to PREPARED on success
 *
 * @session: Session context
 *
 * Return: 0 on success, -1 on error (session state becomes ERROR)
 */
int winafi_session_prepare(winafi_session_t *session);

/**
 * Execute the write operation
 *
 * Synchronous, blocking operation that performs the complete write sequence:
 * 1. Wipe device partition table (0%)
 * 2. Create MBR partition table (10%)
 * 3. Create FAT32 boot partition (20%)
 * 4. Create NTFS data partition (30%)
 * 5. Format FAT32 partition (40%)
 * 6. Format NTFS partition (50%)
 * 7. Create temporary mount directories (60%)
 * 8. Mount partitions (65%)
 * 9. Extract ISO to NTFS partition (90%)
 * 10. Install GRUB BIOS bootloader (95%)
 * 11. Setup UEFI:NTFS bootloader (98%)
 * 12. Sync filesystem (99%)
 * 13. Unmount partitions (100%)
 *
 * The operation is fail-fast: on any error, stops immediately and
 * sets the session error code/message but may leave the device in
 * a partially written state (caller should warn user).
 *
 * State: Can only be called from PREPARED
 * Transitions to EXECUTING, then COMPLETED on success or ERROR on failure
 *
 * @session: Session context
 *
 * Return: 0 on success, -1 on error (session state becomes ERROR)
 */
int winafi_session_execute(winafi_session_t *session);

/**
 * Get last error code
 *
 * Returns the string-format error code (e.g., "E-30-D") of the last
 * error that occurred, or NULL if no error.
 *
 * @session: Session context
 *
 * Return: Pointer to static error code string, or NULL if no error
 *         Caller must NOT free this pointer
 */
const char *winafi_get_error_code(winafi_session_t *session);

/**
 * Get last error message
 *
 * Returns a human-readable error message for the last error that occurred,
 * or NULL if no error.
 *
 * @session: Session context
 *
 * Return: Pointer to allocated error message string, or NULL if no error
 *         Caller MUST free this pointer with free()
 */
const char *winafi_get_error_message(winafi_session_t *session);

/**
 * Set progress callback
 *
 * Registers a callback to receive progress updates during operation.
 * The callback is invoked at major milestones with percentage (0-100)
 * and human-readable message.
 *
 * @session: Session context
 * @callback: Callback function (may be NULL to unregister)
 * @user_data: Opaque data passed to callback
 */
void winafi_set_progress_callback(winafi_session_t *session,
                                 winafi_progress_callback_t callback,
                                 void *user_data);

/*
 * Configure Windows unattended customization for the write (Feature 1/4/5).
 * flags: bitwise-OR of WUE_* constants from wue.h (e.g. WUE_BYPASS_ALL,
 *        WUE_NO_ONLINE_ACCOUNT, WUE_SET_USER). 0 disables customization.
 * username: local account name (used when WUE_SET_USER is set); may be NULL.
 * Returns WINAFI_OK on success, -1 on NULL session.
 */
int winafi_session_set_unattend(winafi_session_t *session, int flags, const char *username);

/*
 * Get the Linux Secure Boot status for the currently loaded ISO.
 * Returns LINUX_SB_UNKNOWN if no ISO is loaded or it is not a Linux ISO.
 * Can be called after winafi_session_load_iso() succeeds.
 */
linux_sb_status_t winafi_get_linux_sb_status(winafi_session_t *session);

#endif
