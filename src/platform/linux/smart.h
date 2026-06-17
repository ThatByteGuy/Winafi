#ifndef WINAFI_SMART_H
#define WINAFI_SMART_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

/* SMART health status */
typedef enum {
    SMART_STATUS_UNKNOWN = 0,
    SMART_STATUS_GOOD,
    SMART_STATUS_BAD,
    SMART_STATUS_PREFAIL  /* threshold exceeded but not failed yet — from SMART attribute analysis */
} smart_status_t;

/* Basic SMART info for a device */
typedef struct {
    smart_status_t status;
    int            temperature_celsius;  /* -1 if unavailable */
    long long      power_on_hours;       /* -1 if unavailable */
    int            reallocated_sectors;  /* -1 if unavailable */
} smart_info_t;

/* Query SMART info for a block device.
   devnode: e.g. "/dev/sda" (not a partition like /dev/sda1)
   info: output struct filled on success
   Returns 0 on success, -1 on error (device not found, no SMART support, etc.)
   NOTE: Requires root (or CAP_SYS_RAWIO) to read SMART data. */
int smart_query(const char *devnode, smart_info_t *info);

/* Compatibility wrapper for callers that use the device-info naming pattern. */
int smart_get_info(const char *devnode, smart_info_t *info);

/* Returns 1 if SMART is supported for this device (best-effort check via sysfs),
   0 if not supported or unknown, -1 on error. */
int smart_is_supported(const char *devnode);

#ifdef __cplusplus
}
#endif
#endif /* WINAFI_SMART_H */
