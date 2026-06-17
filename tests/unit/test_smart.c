// tests/unit/test_smart.c
#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include "platform/linux/smart.h"

static void test_smart_query_nonexistent_returns_error(void) {
    smart_info_t info = {0};
    assert(smart_get_info("/dev/nonexistent_xyz_device", &info) == -1);
    (void)info;  // Avoid unused variable warning
}

static void test_smart_info_struct_fields(void) {
    smart_info_t info = {0};
    assert(info.status == SMART_STATUS_UNKNOWN);
    assert(info.reallocated_sectors == 0);
    (void)info;  // Avoid unused variable warning
}

int main(void) {
    test_smart_query_nonexistent_returns_error();
    test_smart_info_struct_fields();
    printf("All smart tests passed\n");
    return 0;
}
