#define _POSIX_C_SOURCE 200809L
#include "smart.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>

#ifdef HAVE_LIBATASMART
#include <atasmart.h>
#include <stdint.h>

/* Callback to parse SMART attributes: extract reallocated_sectors and detect prefail */
static void smart_attr_callback(SkDisk *d, const SkSmartAttributeParsedData *a, void *userdata)
{
    (void)d;  /* Unused */
    smart_info_t *info = (smart_info_t *)userdata;

    if (a->id == 5) {
        /* Attribute 5 = Reallocated Sector Count */
        info->reallocated_sectors = (int)a->current_value;
    }

    /* Detect prefail: if attribute crossed threshold but drive not dead yet */
    if (a->good_in_the_past == TRUE && a->good_now == FALSE && info->status == SMART_STATUS_GOOD) {
        info->status = SMART_STATUS_PREFAIL;
    }
}

int smart_is_supported(const char *devnode)
{
    if (!devnode) return -1;
    SkDisk *d = NULL;
    if (sk_disk_open(devnode, &d) < 0) return 0;
    SkBool available = FALSE;
    sk_disk_smart_is_available(d, &available);
    sk_disk_free(d);
    return available ? 1 : 0;
}

int smart_query(const char *devnode, smart_info_t *info)
{
    if (!devnode || !info) return -1;
    memset(info, 0, sizeof(*info));
    info->temperature_celsius = -1;
    info->power_on_hours      = -1;
    info->reallocated_sectors = -1;

    SkDisk *d = NULL;
    if (sk_disk_open(devnode, &d) < 0) return -1;
    if (sk_disk_smart_read_data(d) < 0) {
        sk_disk_free(d);
        return -1;
    }

    SkBool good = FALSE;
    if (sk_disk_smart_status(d, &good) >= 0)
        info->status = good ? SMART_STATUS_GOOD : SMART_STATUS_BAD;

    /* Parse attributes to extract reallocated_sectors and detect prefail */
    sk_disk_smart_parse_attributes(d, smart_attr_callback, info);

    uint64_t temp_mk = 0;
    if (sk_disk_smart_get_temperature(d, &temp_mk) >= 0)
        info->temperature_celsius = (int)((int64_t)(temp_mk / 1000U) - 273);

    uint64_t hours = 0;
    if (sk_disk_smart_get_power_on(d, &hours) >= 0)
        info->power_on_hours = (long long)hours;

    sk_disk_free(d);
    return 0;
}

#else /* HAVE_LIBATASMART — sysfs fallback */

/* Extract the device basename from a devnode like "/dev/sda" -> "sda". */
static const char *devnode_basename(const char *devnode)
{
    const char *p = strrchr(devnode, '/');
    return p ? p + 1 : devnode;
}

int smart_is_supported(const char *devnode)
{
    if (!devnode) return -1;

    const char *name = devnode_basename(devnode);
    char path[256];
    /* Check that the device exists in sysfs */
    int n = snprintf(path, sizeof(path), "/sys/block/%s", name);
    if (n < 0 || (size_t)n >= sizeof(path)) return -1;

    FILE *f = fopen(path, "r");
    if (f) {
        fclose(f);
        /* sysfs entry exists — we can't tell if SMART is supported without
           issuing an ATA command, so return 0 (unknown / not confirmed). */
        return 0;
    }
    /* Directory open via fopen fails; try stat-like approach via size file */
    char size_path[256];
    n = snprintf(size_path, sizeof(size_path), "/sys/block/%s/size", name);
    if (n < 0 || (size_t)n >= sizeof(size_path)) return -1;

    f = fopen(size_path, "r");
    if (f) {
        fclose(f);
        return 0; /* device exists but SMART status unknown without libatasmart */
    }
    return -1; /* device not found */
}

int smart_query(const char *devnode, smart_info_t *info)
{
    if (!devnode || !info) return -1;
    memset(info, 0, sizeof(*info));
    info->temperature_celsius = -1;
    info->power_on_hours      = -1;
    info->reallocated_sectors = -1;
    info->status              = SMART_STATUS_UNKNOWN;

    /* Verify the device exists in sysfs */
    const char *name = devnode_basename(devnode);
    char size_path[256];
    int n = snprintf(size_path, sizeof(size_path), "/sys/block/%s/size", name);
    if (n < 0 || (size_t)n >= sizeof(size_path)) return -1;

    FILE *f = fopen(size_path, "r");
    if (!f) return -1; /* device not found */
    fclose(f);

    /* Without libatasmart we cannot read SMART attributes — return success
       with UNKNOWN status and -1 for all numeric fields. */
    return 0;
}

#endif /* HAVE_LIBATASMART */

int smart_get_info(const char *devnode, smart_info_t *info)
{
    return smart_query(devnode, info);
}
