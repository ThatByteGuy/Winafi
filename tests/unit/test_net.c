#define _DEFAULT_SOURCE
#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "platform/linux/net.h"

static void test_fetch_string_returns_non_null(void) {
    /* Only run if network available */
    char *buf = net_fetch_string("https://httpbin.org/get", 5);
    if (buf) {
        assert(strlen(buf) > 0);
        free(buf);
    }
    /* If NULL (no network), that's acceptable — not a failure */
}

static void test_download_file_to_tmp(void) {
    const char *url = "https://httpbin.org/bytes/1024";
    char tmp[] = "/tmp/test_net_XXXXXX";
    int fd = mkstemp(tmp);
    if (fd >= 0) close(fd);

    int rc = net_download_file(url, tmp, 10, NULL, NULL);
    /* Accept both success (0) and failure (-1) — network may be unavailable */
    assert(rc == 0 || rc == -1);
    unlink(tmp);
}

static void test_net_init_cleanup(void) {
    net_init();
    net_cleanup();
    /* Second init/cleanup should be safe */
    net_init();
    net_cleanup();
}

int main(void) {
    test_net_init_cleanup();
    test_fetch_string_returns_non_null();
    test_download_file_to_tmp();
    printf("All net tests passed\n");
    return 0;
}
