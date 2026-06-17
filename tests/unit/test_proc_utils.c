/*
 * test_proc_utils.c - Test suite for /proc filesystem utilities
 */
#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include "platform/linux/proc_utils.h"

static void test_running_as_root_returns_valid(void)
{
    /* Function must return 0 or 1, never crash */
    int r = proc_running_as_root();
    assert(r == 0 || r == 1);
}

static void test_device_not_open_nonexistent(void)
{
    /* A device that doesn't exist should return 0 (no PID holds it) */
    int pid = proc_device_open_by_pid("/dev/nonexistent_device_xyz");
    assert(pid == 0);
}

static void test_device_open_by_pid_self(void)
{
    /* Open /dev/null (always present), scan — we may or may not see it
     * but the function must not crash */
    int pid = proc_device_open_by_pid("/dev/null");
    assert(pid >= 0);
}

static void test_device_is_mounted_nonexistent(void)
{
    /* A device that doesn't exist should return 0 */
    int result = proc_device_is_mounted("/dev/nonexistent_device_xyz");
    assert(result == 0);
}

static void test_proc_device_open_by_pid_null_param(void)
{
    /* NULL parameter should return -1 on error */
    int pid = proc_device_open_by_pid(NULL);
    assert(pid == -1);
}

static void test_proc_device_is_mounted_null_param(void)
{
    /* NULL parameter should return 0 */
    int result = proc_device_is_mounted(NULL);
    assert(result == 0);
}

int main(void)
{
    test_running_as_root_returns_valid();
    test_device_not_open_nonexistent();
    test_device_open_by_pid_self();
    test_device_is_mounted_nonexistent();
    test_proc_device_open_by_pid_null_param();
    test_proc_device_is_mounted_null_param();
    printf("All proc_utils tests passed\n");
    return 0;
}
