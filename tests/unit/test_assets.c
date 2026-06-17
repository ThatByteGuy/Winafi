#include "assets.h"
#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    char path[4096];
    // Env override takes priority
    setenv("WINAFI_DATADIR", "/tmp/winafi_assets_test", 1);
    system("mkdir -p /tmp/winafi_assets_test/uefi-ntfs && touch /tmp/winafi_assets_test/uefi-ntfs/bootx64.efi");
    assert(assets_find("uefi-ntfs/bootx64.efi", path, sizeof(path)) == 0);
    assert(strcmp(path, "/tmp/winafi_assets_test/uefi-ntfs/bootx64.efi") == 0);
    // Missing asset -> -1
    assert(assets_find("does/not/exist.bin", path, sizeof(path)) == -1);
    printf("test_assets OK\n");
    return 0;
}
