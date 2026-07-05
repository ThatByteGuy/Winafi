#ifndef WINAFI_DEVICE_H
#define WINAFI_DEVICE_H

#include "winafi.h"  // For winafi_device_t definition

typedef struct winafi_device_context winafi_device_context_t;

// Initialize device context (creates libudev context)
winafi_device_context_t *device_init(void);

// Cleanup device context
void device_cleanup(winafi_device_context_t *ctx);

// Enumerate all disk-type block devices (not partitions, not virtual).
// Returns devices with is_removable, is_mounted, and mount_point correctly set.
// Caller must free *devices via device_free_list()
int device_enumerate(winafi_device_context_t *ctx,
                    winafi_device_t **devices,
                    int *device_count);

// Free enumeration result
void device_free_list(winafi_device_t *devices);

// Get device info (single device by devnode)
int device_get_info(winafi_device_context_t *ctx,
                   const char *devnode,
                   winafi_device_t *out_device);

// Check if a devnode is mounted (checks /proc/mounts)
int device_is_mounted(const char *devnode);

// Safety-validate a device for destructive operations.
// Checks: exists, block device, not system disk, not locked.
// Does NOT check removable or mounted status (GUI handles that).
int device_validate(const char *devnode);

#endif
