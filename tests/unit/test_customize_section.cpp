#include "sections/WindowsCustomizeSection.h"
#include "platform/linux/wue.h"
#undef NDEBUG
#include <QApplication>
#include <QCheckBox>
#include <cassert>
#include <cstdio>

int main(int argc, char **argv) {
    QApplication app(argc, argv);              // needs QT_QPA_PLATFORM=offscreen
    WindowsCustomizeSection sec;
    assert(sec.wueFlags() == 0);
    QCheckBox *all = sec.findChild<QCheckBox*>("applyAll");
    assert(all != nullptr);
    all->setChecked(true);
    assert((sec.wueFlags() & WUE_BYPASS_ALL) == WUE_BYPASS_ALL);
    all->setChecked(false);
    QCheckBox *tpm = sec.findChild<QCheckBox*>("bypassTpm");
    assert(tpm != nullptr);
    tpm->setChecked(true);
    assert(sec.wueFlags() == WUE_BYPASS_TPM);
    std::puts("test_customize_section OK");
    return 0;
}
