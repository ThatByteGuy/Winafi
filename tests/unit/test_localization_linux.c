#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "platform/linux/localization_linux.h"

static void test_get_string_returns_key_when_no_locale_loaded(void) {
    // Without any locale loaded, must return the key itself (fallback)
    const char *val = loc_get_string("MSG_TEST_KEY");
    assert(val != NULL);
    assert(strlen(val) > 0);
}

static void test_load_locale_from_string(void) {
    // Load a mini locale from a string
    const char *mini = "MSG_HELLO=Hello World\nMSG_BYE=Goodbye\n";
    int rc = loc_load_from_string(mini);
    assert(rc == 0);
    assert(strcmp(loc_get_string("MSG_HELLO"), "Hello World") == 0);
    assert(strcmp(loc_get_string("MSG_BYE"), "Goodbye") == 0);
    loc_unload();
}

int main(void) {
    test_get_string_returns_key_when_no_locale_loaded();
    test_load_locale_from_string();
    printf("All localization_linux tests passed\n");
    return 0;
}
