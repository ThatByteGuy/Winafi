#define _GNU_SOURCE
#include "assets.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <libgen.h>
#include <stdarg.h>

static int exists(const char *p){ return access(p, R_OK) == 0; }

static int try_path(char *out, unsigned long n, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vsnprintf(out, n, fmt, ap); va_end(ap);
    return exists(out);
}

int assets_find(const char *rel, char *out, unsigned long out_size) {
    if (!rel || !out) return -1;
    const char *datadir = getenv("WINAFI_DATADIR");
    if (datadir && try_path(out, out_size, "%s/%s", datadir, rel)) return 0;

    char exe[PATH_MAX]; ssize_t len = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (len > 0) {
        exe[len] = '\0';
        char dir[PATH_MAX]; snprintf(dir, sizeof(dir), "%s", exe);
        char *d = dirname(dir);
        if (try_path(out, out_size, "%s/../share/winafi/assets/%s", d, rel)) return 0;
        if (try_path(out, out_size, "%s/../src/assets/%s", d, rel)) return 0;
        if (try_path(out, out_size, "%s/../../src/assets/%s", d, rel)) return 0;
    }
    if (try_path(out, out_size, "src/assets/%s", rel)) return 0;
    return -1;
}
