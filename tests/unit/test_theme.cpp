#include "theme/Theme.h"
#undef NDEBUG
#include <cassert>
#include <cstdio>
int main() {
    QString dark = winafi::themeStylesheet(true);
    QString light = winafi::themeStylesheet(false);
    assert(dark.contains("#8b5cf6"));
    assert(light.contains("#8b5cf6"));
    assert(dark.contains("#1c1830"));
    assert(!light.contains("#1c1830"));
    assert(dark.contains("QPushButton"));
    assert(dark.contains("QComboBox"));
    std::puts("test_theme OK");
    return 0;
}
