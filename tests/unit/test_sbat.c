#include "sbat.h"
#undef NDEBUG
#include <assert.h>
#include <stdio.h>

int main(void) {
    // Missing file -> error
    assert(sbat_validate("/tmp/nonexistent_efi_xyz_12345.efi") == -1);
    // NULL -> error
    assert(sbat_validate(NULL) == -1);
    // Non-PE text file -> error (NOT silently 0)
    FILE *f = fopen("/tmp/winafi_sbat_text.bin", "wb");
    fputs("NOT_A_PE_FILE", f); fclose(f);
    assert(sbat_validate("/tmp/winafi_sbat_text.bin") == -1);
    // Real signed loader -> valid PE: must be 0 or 1, never -1
    int r = sbat_validate("src/assets/uefi-ntfs/bootx64.efi");
    assert(r == 0 || r == 1);
    printf("test_sbat OK\n");
    return 0;
}
