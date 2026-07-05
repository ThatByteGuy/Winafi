#include "device.h"
#include "device_validate.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libudev.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/sysmacros.h>
#include <dirent.h>
#include <linux/limits.h>

#define WINAFI_DEVICE_ERROR_INIT -1
#define WINAFI_DEVICE_ERROR_ENUM -2
#define WINAFI_DEVICE_ERROR_MEMORY -3
#define WINAFI_DEVICE_ERROR_INVALID -4

typedef struct winafi_device_context {
    struct udev *udev;
} winafi_device_context_t;

winafi_device_context_t *device_init(void) {
    winafi_device_context_t *ctx = malloc(sizeof(*ctx));
    if (!ctx) {
        fprintf(stderr, "Failed to allocate device context\n");
        return NULL;
    }

    ctx->udev = udev_new();
    if (!ctx->udev) {
        fprintf(stderr, "Failed to create udev context\n");
        free(ctx);
        return NULL;
    }

    return ctx;
}

void device_cleanup(winafi_device_context_t *ctx) {
    if (!ctx) return;
    if (ctx->udev) {
        udev_unref(ctx->udev);
        ctx->udev = NULL;
    }
    free(ctx);
}

static const char *device_get_property(struct udev_device *dev,
                                       const char *name,
                                       const char *default_val) {
    const char *val = udev_device_get_property_value(dev, name);
    return val ? val : default_val;
}

static int device_is_usb_device(struct udev_device *dev) {
    const char *usb_vendor = udev_device_get_property_value(dev, "ID_USB_VENDOR");
    if (usb_vendor) {
        return 1;
    }
    struct udev_device *parent = udev_device_get_parent_with_subsystem_devtype(
        dev, "usb", "usb_device");
    return (parent != NULL) ? 1 : 0;
}

static uint64_t device_get_capacity(struct udev_device *dev) {
    const char *sysname = udev_device_get_sysname(dev);
    if (!sysname) return 0;

    char sysfs_path[256];
    snprintf(sysfs_path, sizeof(sysfs_path),
             "/sys/block/%s/size", sysname);

    FILE *fp = fopen(sysfs_path, "r");
    if (!fp) return 0;

    uint64_t sectors = 0;
    if (fscanf(fp, "%" PRIu64, &sectors) != 1) {
        fclose(fp);
        return 0;
    }
    fclose(fp);

    return sectors * 512;
}

int device_is_mounted(const char *devnode) {
    if (!devnode) return 0;

    FILE *fp = fopen("/proc/mounts", "r");
    if (!fp) return 0;

    char line[4096];
    size_t n = strlen(devnode);
    int ret = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, devnode, n) == 0 &&
            (line[n] == ' ' || line[n] == '\t' || line[n] == '\0')) {
            ret = 1;
            break;
        }
    }
    fclose(fp);
    return ret;
}

static int device_find_mount_point(const char *devnode, char *out, size_t out_size) {
    if (!devnode || !out || out_size == 0) return -1;

    FILE *fp = fopen("/proc/mounts", "r");
    if (!fp) return -1;

    char line[4096];
    size_t n = strlen(devnode);
    int found = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, devnode, n) == 0 &&
            (line[n] == ' ' || line[n] == '\t' || line[n] == '\0')) {
            char mnt_dev[256], mnt_point[256], mnt_type[64], mnt_opts[256];
            int dummy1, dummy2;
            if (sscanf(line, "%255s %255s %63s %255s %d %d",
                       mnt_dev, mnt_point, mnt_type, mnt_opts, &dummy1, &dummy2) >= 2) {
                strncpy(out, mnt_point, out_size - 1);
                out[out_size - 1] = '\0';
                found = 1;
            }
            break;
        }
    }
    fclose(fp);
    return found ? 0 : -1;
}

static int device_is_whole_disk_mounted(const char *sysname) {
    DIR *dp;
    struct dirent *de;
    char path[PATH_MAX];

    snprintf(path, sizeof(path), "/sys/block/%s", sysname);
    dp = opendir(path);
    if (!dp) return 0;

    int mounted = 0;
    while ((de = readdir(dp)) != NULL) {
        if (de->d_name[0] == '.') continue;

        char part_attr[PATH_MAX];
        if (snprintf(part_attr, sizeof(part_attr), "%s/%s/partition", path, de->d_name) < 0 ||
            strlen(path) + 1 + strlen(de->d_name) + strlen("/partition") >= sizeof(part_attr)) {
            continue;
        }
        if (access(part_attr, F_OK) != 0) continue;

        char part_dev[PATH_MAX];
        if (snprintf(part_dev, sizeof(part_dev), "/dev/%s", de->d_name) < 0 ||
            strlen("/dev/") + strlen(de->d_name) >= sizeof(part_dev)) {
            continue;
        }

        if (device_is_mounted(part_dev)) {
            mounted = 1;
            break;
        }
    }
    closedir(dp);
    return mounted;
}

static int device_find_first_mount_point(const char *sysname, char *out, size_t out_size) {
    DIR *dp;
    struct dirent *de;
    char path[PATH_MAX];

    snprintf(path, sizeof(path), "/sys/block/%s", sysname);
    dp = opendir(path);
    if (!dp) return -1;

    int found = 0;
    while ((de = readdir(dp)) != NULL) {
        if (de->d_name[0] == '.') continue;

        char part_attr[PATH_MAX];
        if (snprintf(part_attr, sizeof(part_attr), "%s/%s/partition", path, de->d_name) < 0 ||
            strlen(path) + 1 + strlen(de->d_name) + strlen("/partition") >= sizeof(part_attr)) {
            continue;
        }
        if (access(part_attr, F_OK) != 0) continue;

        char part_dev[PATH_MAX];
        if (snprintf(part_dev, sizeof(part_dev), "/dev/%s", de->d_name) < 0 ||
            strlen("/dev/") + strlen(de->d_name) >= sizeof(part_dev)) {
            continue;
        }

        if (device_find_mount_point(part_dev, out, out_size) == 0) {
            found = 1;
            break;
        }
    }
    closedir(dp);
    return found ? 0 : -1;
}

static int device_should_skip(struct udev_device *dev) {
    const char *dn = udev_device_get_devnode(dev);
    if (!dn) return 1;
    if (strncmp(dn, "/dev/loop", 9) == 0) return 1;
    if (strncmp(dn, "/dev/ram", 8) == 0) return 1;
    if (strncmp(dn, "/dev/dm-", 8) == 0) return 1;
    if (strncmp(dn, "/dev/zram", 9) == 0) return 1;
    if (strncmp(dn, "/dev/nbd", 8) == 0) return 1;
    return 0;
}

/**
 * device_enumerate - Enumerate all disk-type block devices
 *
 * Returns ALL whole-disk block devices (not partitions, not virtual)
 * with correct is_removable, is_mounted, and mount_point fields.
 * The caller (GUI) filters by is_removable + user preference.
 *
 * @ctx: Device context
 * @devices: Output pointer to device array (caller must free)
 * @device_count: Output count of devices found
 *
 * Return: 0 on success, negative on error
 */
int device_enumerate(winafi_device_context_t *ctx,
                    winafi_device_t **devices,
                    int *device_count) {
    if (!ctx || !ctx->udev || !devices || !device_count) {
        fprintf(stderr, "Invalid arguments to device_enumerate\n");
        return WINAFI_DEVICE_ERROR_INVALID;
    }

    struct udev_enumerate *enumerate = udev_enumerate_new(ctx->udev);
    if (!enumerate) {
        fprintf(stderr, "Failed to create udev enumerate\n");
        return WINAFI_DEVICE_ERROR_ENUM;
    }

    udev_enumerate_add_match_subsystem(enumerate, "block");
    udev_enumerate_add_nomatch_sysattr(enumerate, "partition", NULL);

    if (udev_enumerate_scan_devices(enumerate) < 0) {
        fprintf(stderr, "Failed to scan udev devices\n");
        udev_enumerate_unref(enumerate);
        return WINAFI_DEVICE_ERROR_ENUM;
    }

    int count = 0;
    struct udev_list_entry *devices_list = udev_enumerate_get_list_entry(enumerate);
    struct udev_list_entry *entry = NULL;
    udev_list_entry_foreach(entry, devices_list) {
        const char *syspath = udev_list_entry_get_name(entry);
        struct udev_device *dev = udev_device_new_from_syspath(ctx->udev, syspath);
        if (!dev) continue;

        if (device_should_skip(dev)) {
            udev_device_unref(dev);
            continue;
        }

        const char *devnode = udev_device_get_devnode(dev);
        uint64_t capacity = device_get_capacity(dev);
        if (capacity > 0 && devnode) {
            count++;
        }
        udev_device_unref(dev);
    }

    if (count == 0) {
        *devices = NULL;
        *device_count = 0;
        udev_enumerate_unref(enumerate);
        return 0;
    }

    *devices = malloc((size_t)count * sizeof(**devices));
    if (!*devices) {
        fprintf(stderr, "Failed to allocate device array\n");
        udev_enumerate_unref(enumerate);
        return WINAFI_DEVICE_ERROR_MEMORY;
    }

    int idx = 0;
    devices_list = udev_enumerate_get_list_entry(enumerate);
    udev_list_entry_foreach(entry, devices_list) {
        if (idx >= count) break;

        const char *syspath = udev_list_entry_get_name(entry);
        struct udev_device *dev = udev_device_new_from_syspath(ctx->udev, syspath);
        if (!dev) continue;

        if (device_should_skip(dev)) {
            udev_device_unref(dev);
            continue;
        }

        uint64_t capacity = device_get_capacity(dev);
        const char *devnode = udev_device_get_devnode(dev);
        if (capacity == 0 || !devnode) {
            udev_device_unref(dev);
            continue;
        }

        winafi_device_t *devinfo = &(*devices)[idx];
        memset(devinfo, 0, sizeof(*devinfo));

        const char *sysname = udev_device_get_sysname(dev);

        if (devnode) {
            strncpy(devinfo->devnode, devnode, sizeof(devinfo->devnode) - 1);
        }

        if (sysname) {
            strncpy(devinfo->sysname, sysname, sizeof(devinfo->sysname) - 1);
        }

        devinfo->capacity_bytes = capacity;

        const char *vendor = device_get_property(dev, "ID_VENDOR", "Unknown");
        const char *model = device_get_property(dev, "ID_MODEL", "Unknown");
        const char *serial = device_get_property(dev, "ID_SERIAL_SHORT", "Unknown");

        strncpy(devinfo->vendor, vendor, sizeof(devinfo->vendor) - 1);
        strncpy(devinfo->model, model, sizeof(devinfo->model) - 1);
        strncpy(devinfo->serial, serial, sizeof(devinfo->serial) - 1);

        int removable = 0;
        if (device_is_usb_device(dev)) {
            removable = 1;
        } else if (validate_device_is_removable(devnode) == VALIDATE_OK) {
            removable = 1;
        }
        devinfo->is_removable = removable;

        if (sysname) {
            devinfo->is_mounted = device_is_whole_disk_mounted(sysname);
            if (devinfo->is_mounted) {
                device_find_first_mount_point(sysname, devinfo->mount_point,
                                              sizeof(devinfo->mount_point));
            }
        }

        idx++;
        udev_device_unref(dev);
    }

    *device_count = idx;
    udev_enumerate_unref(enumerate);
    return 0;
}

void device_free_list(winafi_device_t *devices) {
    free(devices);
}

int device_get_info(winafi_device_context_t *ctx,
                   const char *devnode,
                   winafi_device_t *out_device) {
    if (!ctx || !ctx->udev || !devnode || !out_device) {
        fprintf(stderr, "Invalid arguments to device_get_info\n");
        return WINAFI_DEVICE_ERROR_INVALID;
    }

    struct stat st;
    if (stat(devnode, &st) != 0) {
        fprintf(stderr, "Failed to stat device %s\n", devnode);
        return WINAFI_DEVICE_ERROR_INVALID;
    }

    if (!S_ISBLK(st.st_mode)) {
        fprintf(stderr, "%s is not a block device\n", devnode);
        return WINAFI_DEVICE_ERROR_INVALID;
    }

    struct udev_device *dev = udev_device_new_from_devnum(ctx->udev, 'b', st.st_rdev);
    if (!dev) {
        fprintf(stderr, "Failed to get device info for %s\n", devnode);
        return WINAFI_DEVICE_ERROR_INVALID;
    }

    memset(out_device, 0, sizeof(*out_device));
    strncpy(out_device->devnode, devnode, sizeof(out_device->devnode) - 1);

    const char *sysname = udev_device_get_sysname(dev);
    if (sysname) {
        strncpy(out_device->sysname, sysname, sizeof(out_device->sysname) - 1);
    }

    uint64_t capacity = device_get_capacity(dev);
    out_device->capacity_bytes = capacity;

    const char *vendor = device_get_property(dev, "ID_VENDOR", "Unknown");
    const char *model = device_get_property(dev, "ID_MODEL", "Unknown");
    const char *serial = device_get_property(dev, "ID_SERIAL_SHORT", "Unknown");

    strncpy(out_device->vendor, vendor, sizeof(out_device->vendor) - 1);
    strncpy(out_device->model, model, sizeof(out_device->model) - 1);
    strncpy(out_device->serial, serial, sizeof(out_device->serial) - 1);

    int removable = 0;
    if (device_is_usb_device(dev)) {
        removable = 1;
    } else if (validate_device_is_removable(devnode) == VALIDATE_OK) {
        removable = 1;
    }
    out_device->is_removable = removable;

    if (sysname) {
        out_device->is_mounted = device_is_whole_disk_mounted(sysname);
        if (out_device->is_mounted) {
            device_find_first_mount_point(sysname, out_device->mount_point,
                                          sizeof(out_device->mount_point));
        }
    }

    udev_device_unref(dev);
    return 0;
}

/**
 * device_validate - Safety-check a device for destructive operations
 *
 * Checks:
 *  - Device exists and is a block device
 *  - Device is NOT the system disk (contains root/boot/etc.)
 *  - Device is not locked by another process
 *
 * NOTE: Removable and mounted checks are removed from here.
 * Removable filtering is done in the GUI layer.
 * Mounted checks are done in session_execute() as a final safety net.
 *
 * @devnode: Device node path
 *
 * Return: 0 if valid/usable, negative if invalid/unsafe
 */
int device_validate(const char *devnode) {
    if (!devnode) {
        fprintf(stderr, "Invalid device node\n");
        return WINAFI_DEVICE_ERROR_INVALID;
    }

    // Check if device exists
    struct stat st;
    if (stat(devnode, &st) != 0) {
        fprintf(stderr, "Device %s does not exist\n", devnode);
        return WINAFI_DEVICE_ERROR_INVALID;
    }

    // Verify it's a block device
    if (!S_ISBLK(st.st_mode)) {
        fprintf(stderr, "%s is not a block device\n", devnode);
        return WINAFI_DEVICE_ERROR_INVALID;
    }

    // Check if system disk (contains root, /boot, /home, /var, or /usr)
    if (validate_not_system_drive(devnode) == VALIDATE_ERR_SYSTEM_DRIVE) {
        fprintf(stderr, "Device %s contains a protected system mount\n", devnode);
        return WINAFI_DEVICE_ERROR_INVALID;
    }

    // Check if locked (only when root, since non-root can't lock anyway)
    if (geteuid() == 0 && validate_device_not_locked(devnode) != VALIDATE_OK) {
        fprintf(stderr, "Device %s is locked or busy\n", devnode);
        return WINAFI_DEVICE_ERROR_INVALID;
    }

    return 0;
}
