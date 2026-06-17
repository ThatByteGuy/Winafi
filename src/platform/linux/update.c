#define _POSIX_C_SOURCE 200809L
#include "update.h"
#include "net.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* WINAFI_VERSION is defined via CMake compile definition */
#ifndef WINAFI_VERSION
#define WINAFI_VERSION "0.0.0"
#endif

int update_strip_v(const char *version, char *out_buf, size_t out_size) {
    if (!version || !out_buf || out_size == 0) return -1;
    if (version[0] == 'v' || version[0] == 'V') version++;
    size_t len = strlen(version);
    if (len >= out_size) return -1;
    memcpy(out_buf, version, len + 1);
    return 0;
}

static int parse_version_components(const char *version, int out[3]) {
    if (!version || !out || version[0] == '\0') return -1;

    int component = 0;
    const unsigned char *p = (const unsigned char *)version;

    while (*p) {
        if (component >= 3 || !isdigit(*p)) return -1;

        int value = 0;
        while (*p && isdigit(*p)) {
            value = (value * 10) + (*p - '0');
            p++;
        }
        out[component++] = value;

        if (*p == '\0') break;
        if (*p != '.') return -1;
        p++;
        if (*p == '\0') return -1;
    }

    return 0;
}

int update_version_compare(const char *a, const char *b) {
    if (!a || !b) return -2;
    int va[3] = {0,0,0}, vb[3] = {0,0,0};
    char ta[64], tb[64];
    if (update_strip_v(a, ta, sizeof(ta)) < 0) return -2;
    if (update_strip_v(b, tb, sizeof(tb)) < 0) return -2;
    if (parse_version_components(ta, va) < 0) return -2;
    if (parse_version_components(tb, vb) < 0) return -2;
    for (int i = 0; i < 3; i++) {
        if (va[i] != vb[i]) return va[i] - vb[i];
    }
    return 0;
}

int update_build_url(const char *tag, char *out_buf, size_t out_size) {
    if (!tag || !out_buf || out_size == 0) return -1;
    char clean[64];
    if (update_strip_v(tag, clean, sizeof(clean)) < 0) return -1;
    int n = snprintf(out_buf, out_size,
        "https://github.com/AlphaGlider25/Winafi/releases/tag/v%s", clean);
    if (n < 0 || (size_t)n >= out_size) return -1;
    return 0;
}

int update_check(char *latest_out, size_t latest_out_size) {
    if (!latest_out || latest_out_size < 2) return -1;
    char *latest = net_check_latest_version();
    if (!latest) return -1;
    char clean_latest[64], clean_current[64];
    int r = -1;
    if (update_strip_v(latest, clean_latest, sizeof(clean_latest)) == 0 &&
        update_strip_v(WINAFI_VERSION, clean_current, sizeof(clean_current)) == 0) {
        int cmp = update_version_compare(clean_latest, clean_current);
        r = (cmp > 0) ? 1 : 0;
        snprintf(latest_out, latest_out_size, "%s", clean_latest);
    }
    free(latest);
    return r;
}
