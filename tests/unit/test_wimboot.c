// tests/unit/test_wimboot.c
#define _POSIX_C_SOURCE 200809L
#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "platform/linux/wimboot.h"

static void test_wimboot_setup_no_efi_file(void) {
    char tmpdir[] = "/tmp/wimboot_test_XXXXXX";
    assert(mkdtemp(tmpdir) != NULL);

    // Without a wimboot.efi asset, returns error
    int rc = wimboot_setup_uefi(tmpdir, "/nonexistent/wimboot.efi");
    assert(rc == -1);
}

static void test_wimboot_setup_with_mock_efi(void) {
    char tmpdir[] = "/tmp/wimboot_test_XXXXXX";
    assert(mkdtemp(tmpdir) != NULL);

    // Create a mock wimboot.efi
    char fake_efi[] = "/tmp/fake_wimboot_XXXXXX";
    int fd = mkstemp(fake_efi);
    assert(fd >= 0);
    assert(write(fd, "MOCKWIMBOOT", 11) == 11);
    close(fd);

    int rc = wimboot_setup_uefi(tmpdir, fake_efi);
    assert(rc == 0);

    // Verify bootx64.efi was placed
    char dst[512];
    snprintf(dst, sizeof(dst), "%s/EFI/Boot/bootx64.efi", tmpdir);
    assert(access(dst, F_OK) == 0);

    unlink(fake_efi);
}

int main(void) {
    test_wimboot_setup_no_efi_file();
    test_wimboot_setup_with_mock_efi();
    printf("All wimboot tests passed\n");
    return 0;
}
