#undef NDEBUG
#include <cassert>
#include <cstdio>
#include <QApplication>
#include "gui/dark_mode.h"

// Mock application pointer for testing
static QApplication* g_test_app = nullptr;

static void test_dark_mode_initially_disabled(void) {
    assert(dark_mode_is_enabled() == false);
}

static void test_dark_mode_enable(void) {
    if (!g_test_app) return;  // Skip if no app instance
    dark_mode_apply(g_test_app, true);
    assert(dark_mode_is_enabled() == true);
}

static void test_dark_mode_disable(void) {
    if (!g_test_app) return;  // Skip if no app instance
    dark_mode_apply(g_test_app, true);
    dark_mode_apply(g_test_app, false);
    assert(dark_mode_is_enabled() == false);
}

static void test_dark_mode_toggle_sequence(void) {
    if (!g_test_app) return;  // Skip if no app instance
    // Sequence: disabled -> enabled -> disabled -> enabled
    assert(dark_mode_is_enabled() == false);
    dark_mode_apply(g_test_app, true);
    assert(dark_mode_is_enabled() == true);
    dark_mode_apply(g_test_app, false);
    assert(dark_mode_is_enabled() == false);
    dark_mode_apply(g_test_app, true);
    assert(dark_mode_is_enabled() == true);
}

int main(int argc, char *argv[]) {
    // Create minimal QApplication for palette testing
    QApplication app(argc, argv);
    g_test_app = &app;

    test_dark_mode_initially_disabled();
    test_dark_mode_enable();
    test_dark_mode_disable();
    test_dark_mode_toggle_sequence();

    printf("All dark_mode tests passed\n");
    return 0;
}
