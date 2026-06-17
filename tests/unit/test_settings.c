#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "platform/linux/settings.h"

static void test_settings_set_get_string(void) {
    settings_t *s = settings_open();
    assert(s != NULL);
    settings_set_string(s, "last_iso_path", "/tmp/test.iso");
    char buf[256];
    int rc = settings_get_string(s, "last_iso_path", buf, sizeof(buf), "");
    assert(rc == 0);
    assert(strcmp(buf, "/tmp/test.iso") == 0);
    settings_close(s);
}

static void test_settings_set_get_int(void) {
    settings_t *s = settings_open();
    assert(s != NULL);
    settings_set_int(s, "partition_scheme", 1);
    int val = settings_get_int(s, "partition_scheme", 0);
    assert(val == 1);
    settings_close(s);
}

static void test_settings_default_returned_when_missing(void) {
    settings_t *s = settings_open();
    assert(s != NULL);
    int val = settings_get_int(s, "nonexistent_key_xyz", 42);
    assert(val == 42);
    settings_close(s);
}

int main(void) {
    test_settings_set_get_string();
    test_settings_set_get_int();
    test_settings_default_returned_when_missing();
    printf("All settings tests passed\n");
    return 0;
}
