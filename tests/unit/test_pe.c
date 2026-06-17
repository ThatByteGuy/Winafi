#include "pe.h"
#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    unsigned char *buf = NULL; size_t len = 0;
    // Non-PE input (text) must return -1
    FILE *f = fopen("/tmp/winafi_pe_text.bin", "wb");
    fwrite("not a pe file", 1, 13, f); fclose(f);
    assert(pe_read_section("/tmp/winafi_pe_text.bin", ".sbat", &buf, &len) == -1);
    // The shipped signed loader must parse as PE (section may or may not exist)
    int r = pe_read_section("src/assets/uefi-ntfs/bootx64.efi", ".sbat", &buf, &len);
    assert(r == 0 || r == 1);   // valid PE either way, not -1
    free(buf);
    printf("test_pe OK\n");
    return 0;
}
