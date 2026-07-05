#ifndef WINAFI_H
#define WINAFI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Error codes
typedef int winafi_error_t;
#define WINAFI_OK 0

// Session management
typedef struct winafi_session winafi_session_t;

// Device enumeration
typedef struct {
    char devnode[64];
    char sysname[32];
    uint64_t capacity_bytes;
    char vendor[256];
    char model[256];
    char serial[256];
    int is_removable;           // 1 if USB/removable, 0 if fixed/internal
    int is_mounted;             // 1 if any partition is currently mounted
    char mount_point[256];      // first mount point if mounted, empty otherwise
} winafi_device_t;

// Session lifecycle
winafi_session_t *winafi_session_create(void);
void winafi_session_destroy(winafi_session_t *session);

// Partition scheme options
typedef enum {
    WINAFI_PARTITION_MBR = 0,
    WINAFI_PARTITION_GPT = 1,
} winafi_partition_scheme_t;

// Target system options
typedef enum {
    WINAFI_TARGET_BIOS = 0,
    WINAFI_TARGET_UEFI = 1,
} winafi_target_system_t;

// File system options
typedef enum {
    WINAFI_FS_FAT32 = 0,
    WINAFI_FS_NTFS = 1,
    WINAFI_FS_EXFAT = 2,
} winafi_filesystem_t;

// Image option (Windows install type)
typedef enum {
    WINAFI_IMAGE_STANDARD = 0,
    WINAFI_IMAGE_PORTABLE = 1,
    WINAFI_IMAGE_VHD = 2,
    WINAFI_IMAGE_WINTOGO = 3,
} winafi_image_option_t;

// Device operations
int winafi_enumerate_devices(winafi_session_t *session,
                           winafi_device_t **devices,
                           int *device_count);

// ISO loading and detection
int winafi_session_load_iso(winafi_session_t *session, const char *iso_path);
const char *winafi_get_detected_os(winafi_session_t *session);
int winafi_get_linux_sb_status(winafi_session_t *session);
winafi_image_option_t winafi_get_image_option(winafi_session_t *session);
int winafi_session_set_image_option(winafi_session_t *session, winafi_image_option_t option);

// Device selection
int winafi_session_select_device(winafi_session_t *session, const char *devnode);

// Format options
int winafi_session_set_partition_scheme(winafi_session_t *session, winafi_partition_scheme_t scheme);
int winafi_session_set_target_system(winafi_session_t *session, winafi_target_system_t target);
int winafi_session_set_filesystem(winafi_session_t *session, winafi_filesystem_t fs);
int winafi_session_set_cluster_size(winafi_session_t *session, uint32_t cluster_size);
int winafi_session_set_volume_label(winafi_session_t *session, const char *label);
int winafi_session_set_quick_format(winafi_session_t *session, int quick);
int winafi_session_set_bad_blocks_check(winafi_session_t *session, int enabled, int passes);

// Windows unattended customization (Features 1/4/5).
// flags: bitwise-OR of WUE_* constants from src/platform/linux/wue.h
// (e.g. WUE_BYPASS_ALL, WUE_NO_ONLINE_ACCOUNT, WUE_SET_USER). 0 disables it.
// username: local account name (used with WUE_SET_USER); may be NULL.
int winafi_session_set_unattend(winafi_session_t *session, int flags, const char *username);

// Get current settings
winafi_partition_scheme_t winafi_session_get_partition_scheme(winafi_session_t *session);
winafi_target_system_t winafi_session_get_target_system(winafi_session_t *session);
winafi_filesystem_t winafi_session_get_filesystem(winafi_session_t *session);
const char *winafi_session_get_volume_label(winafi_session_t *session);
winafi_image_option_t winafi_session_get_image_option(winafi_session_t *session);
int winafi_session_get_bad_blocks_enabled(winafi_session_t *session);
int winafi_session_get_bad_blocks_passes(winafi_session_t *session);

// Preparation
int winafi_session_prepare(winafi_session_t *session);

// Execution with extended callbacks
typedef void (*winafi_progress_callback_ex_t)(int percent, const char *message,
                                             const char *current_file, void *user_data);
int winafi_session_execute(winafi_session_t *session);
int winafi_session_execute_ex(winafi_session_t *session, winafi_progress_callback_ex_t callback, void *user_data);
uint32_t winafi_session_get_elapsed_seconds(winafi_session_t *session);

// Error handling
const char *winafi_get_error_code(winafi_session_t *session);
const char *winafi_get_error_message(winafi_session_t *session);

// Cancel running operation
void winafi_session_cancel(winafi_session_t *session);

// Progress callback
typedef void (*winafi_progress_callback_t)(int percent, const char *message, void *user_data);
void winafi_set_progress_callback(winafi_session_t *session,
                                winafi_progress_callback_t callback,
                                void *user_data);

// Logging
typedef enum {
    WINAFI_LOG_DEBUG = 0,
    WINAFI_LOG_INFO = 1,
    WINAFI_LOG_ERROR = 2
} winafi_log_level_t;

void winafi_set_log_level(winafi_log_level_t level);

#ifdef __cplusplus
}
#endif

#endif
