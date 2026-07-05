#ifndef WINAFI_MOUNT_OPS_H
#define WINAFI_MOUNT_OPS_H

#include <stdint.h>

typedef struct {
    char fat_mount[256];   // /tmp/winafi-mount-XXXX/fat
    char ntfs_mount[256];  // /tmp/winafi-mount-XXXX/ntfs
    char temp_dir[256];    // /tmp/winafi-mount-XXXX
} mount_context_t;

// Create temporary mount points
// Returns: 0 on success, negative on error
int mount_create_temp_dirs(mount_context_t *ctx);

// Mount FAT32 partition
// device: /dev/sdX1
// mount_point: /tmp/winafi-mount-XXXX/fat
int mount_fat32(const char *device, const char *mount_point);

// Mount NTFS partition
// device: /dev/sdX2
// mount_point: /tmp/winafi-mount-XXXX/ntfs
int mount_ntfs(const char *device, const char *mount_point);

// Unmount partition and cleanup
int unmount_and_cleanup(mount_context_t *ctx);

// Sync filesystem buffers
int mount_sync(void);

// Sync only the filesystem containing the given path (faster than global sync)
int mount_sync_path(const char *path);

// Sync with timeout seconds — returns 0 on success, -1 on timeout/failure
int mount_sync_path_timeout(const char *path, int timeout_sec);

#endif
