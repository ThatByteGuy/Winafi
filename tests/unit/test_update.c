#define _DEFAULT_SOURCE
#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "platform/linux/update.h"

/* Test version comparison with various formats */
static void test_update_version_compare_equal(void) {
    assert(update_version_compare("1.0.0", "1.0.0") == 0);
    assert(update_version_compare("4.1.0", "4.1.0") == 0);
    assert(update_version_compare("0.0.0", "0.0.0") == 0);
}

static void test_update_version_compare_less_than(void) {
    assert(update_version_compare("1.0.0", "2.0.0") < 0);
    assert(update_version_compare("1.0.0", "1.1.0") < 0);
    assert(update_version_compare("1.0.0", "1.0.1") < 0);
    assert(update_version_compare("0.0.1", "1.0.0") < 0);
}

static void test_update_version_compare_greater_than(void) {
    assert(update_version_compare("2.0.0", "1.0.0") > 0);
    assert(update_version_compare("1.1.0", "1.0.0") > 0);
    assert(update_version_compare("1.0.1", "1.0.0") > 0);
    assert(update_version_compare("1.0.0", "0.0.1") > 0);
}

static void test_update_version_compare_missing_components(void) {
    /* Versions with missing patch components should be treated as 0 */
    assert(update_version_compare("1.0", "1.0.0") == 0);
    assert(update_version_compare("1.0.0", "1.0") == 0);
    assert(update_version_compare("2", "2.0.0") == 0);
    assert(update_version_compare("2.0.0", "2") == 0);
}

static void test_update_version_compare_leading_v(void) {
    /* Leading 'v' should be stripped internally */
    assert(update_version_compare("v1.0.0", "1.0.0") == 0);
    assert(update_version_compare("1.0.0", "v1.0.0") == 0);
    assert(update_version_compare("v1.0.0", "v1.0.0") == 0);
    assert(update_version_compare("v2.0.0", "v1.0.0") > 0);
}

static void test_update_version_compare_invalid_input(void) {
    /* NULL inputs should return -2 */
    assert(update_version_compare(NULL, "1.0.0") == -2);
    assert(update_version_compare("1.0.0", NULL) == -2);
    assert(update_version_compare(NULL, NULL) == -2);
}

static void test_update_version_compare_non_numeric(void) {
    /* Non-numeric version strings should return -2 */
    assert(update_version_compare("abc", "1.0.0") == -2);
    assert(update_version_compare("1.0.0", "xyz") == -2);
    assert(update_version_compare("", "1.0.0") >= -2);
}

static void test_update_strip_v_with_leading_v(void) {
    char buf[64];
    memset(buf, 0, sizeof(buf));
    assert(update_strip_v("v1.0.0", buf, sizeof(buf)) == 0);
    assert(strcmp(buf, "1.0.0") == 0);
}

static void test_update_strip_v_with_uppercase_v(void) {
    char buf[64];
    memset(buf, 0, sizeof(buf));
    assert(update_strip_v("V1.0.0", buf, sizeof(buf)) == 0);
    assert(strcmp(buf, "1.0.0") == 0);
}

static void test_update_strip_v_without_v(void) {
    char buf[64];
    memset(buf, 0, sizeof(buf));
    assert(update_strip_v("1.0.0", buf, sizeof(buf)) == 0);
    assert(strcmp(buf, "1.0.0") == 0);
}

static void test_update_strip_v_null_input(void) {
    char buf[64];
    memset(buf, 0, sizeof(buf));
    assert(update_strip_v(NULL, buf, sizeof(buf)) == -1);
}

static void test_update_strip_v_null_output(void) {
    assert(update_strip_v("1.0.0", NULL, 64) == -1);
}

static void test_update_strip_v_buffer_too_small(void) {
    char buf[4];
    memset(buf, 0, sizeof(buf));
    /* "1.0.0" is 5 chars + NUL, needs at least 6 bytes */
    assert(update_strip_v("1.0.0", buf, sizeof(buf)) == -1);
}

static void test_update_strip_v_zero_buffer_size(void) {
    assert(update_strip_v("1.0.0", NULL, 0) == -1);
}

static void test_update_build_url_basic(void) {
    char buf[512];
    memset(buf, 0, sizeof(buf));
    assert(update_build_url("v4.1.0", buf, sizeof(buf)) == 0);
    assert(strcmp(buf, "https://github.com/AlphaGlider25/Winafi/releases/tag/v4.1.0") == 0);
}

static void test_update_build_url_without_v(void) {
    char buf[512];
    memset(buf, 0, sizeof(buf));
    assert(update_build_url("4.1.0", buf, sizeof(buf)) == 0);
    assert(strcmp(buf, "https://github.com/AlphaGlider25/Winafi/releases/tag/v4.1.0") == 0);
}

static void test_update_build_url_uppercase_v(void) {
    char buf[512];
    memset(buf, 0, sizeof(buf));
    assert(update_build_url("V4.1.0", buf, sizeof(buf)) == 0);
    assert(strcmp(buf, "https://github.com/AlphaGlider25/Winafi/releases/tag/v4.1.0") == 0);
}

static void test_update_build_url_null_tag(void) {
    char buf[512];
    memset(buf, 0, sizeof(buf));
    assert(update_build_url(NULL, buf, sizeof(buf)) == -1);
}

static void test_update_build_url_null_output(void) {
    assert(update_build_url("4.1.0", NULL, 512) == -1);
}

static void test_update_build_url_buffer_too_small(void) {
    char buf[10];
    memset(buf, 0, sizeof(buf));
    assert(update_build_url("4.1.0", buf, 10) == -1);
}

static void test_update_build_url_zero_buffer(void) {
    assert(update_build_url("4.1.0", NULL, 0) == -1);
}

/* Test update_check with mocked version */
static void test_update_check_null_output(void) {
    assert(update_check(NULL, 64) == -1);
}

static void test_update_check_buffer_too_small(void) {
    char buf[1];
    memset(buf, 0, sizeof(buf));
    /* Should fail because buffer is too small for any output */
    assert(update_check(buf, 1) == -1);
}

/* Helper to run all tests */
static void run_version_comparison_tests(void) {
    test_update_version_compare_equal();
    test_update_version_compare_less_than();
    test_update_version_compare_greater_than();
    test_update_version_compare_missing_components();
    test_update_version_compare_leading_v();
    test_update_version_compare_invalid_input();
    test_update_version_compare_non_numeric();
    printf("All version comparison tests passed\n");
}

static void run_strip_v_tests(void) {
    test_update_strip_v_with_leading_v();
    test_update_strip_v_with_uppercase_v();
    test_update_strip_v_without_v();
    test_update_strip_v_null_input();
    test_update_strip_v_null_output();
    test_update_strip_v_buffer_too_small();
    test_update_strip_v_zero_buffer_size();
    printf("All strip_v tests passed\n");
}

static void run_build_url_tests(void) {
    test_update_build_url_basic();
    test_update_build_url_without_v();
    test_update_build_url_uppercase_v();
    test_update_build_url_null_tag();
    test_update_build_url_null_output();
    test_update_build_url_buffer_too_small();
    test_update_build_url_zero_buffer();
    printf("All build_url tests passed\n");
}

static void run_update_check_tests(void) {
    test_update_check_null_output();
    test_update_check_buffer_too_small();
    printf("All update_check tests passed\n");
}

int main(void) {
    run_version_comparison_tests();
    run_strip_v_tests();
    run_build_url_tests();
    run_update_check_tests();
    printf("All update tests passed!\n");
    return 0;
}
