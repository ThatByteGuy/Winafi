#ifndef WINAFI_WINDOWS_CUSTOMIZE_SECTION_H
#define WINAFI_WINDOWS_CUSTOMIZE_SECTION_H
#include <QWidget>
#include <QString>
class QCheckBox; class QLineEdit; class QLabel; class QStackedWidget;

// Shows OS-specific options: Windows 11 bypasses for Windows ISOs,
// and Secure Boot status for Linux ISOs.
class WindowsCustomizeSection : public QWidget {
    Q_OBJECT
public:
    explicit WindowsCustomizeSection(QWidget *parent = nullptr);
    int wueFlags() const;        // bitwise-OR of WUE_* (see wue.h); 0 if nothing enabled
    QString username() const;    // local account name (empty if "create account" unchecked)
    void setWindowsIso(bool isWindows);  // show Windows options
    // Show Linux Secure Boot status (sbStatus: 0=unknown,1=shim,2=signed,3=unsigned)
    void setLinuxIso(int sbStatus, const QString &distroHint = QString());
signals:
    void changed();
private slots:
    void onApplyAllToggled(int state);
private:
    QStackedWidget *m_stack;
    // Windows page (index 0)
    QWidget *m_winPage;
    QCheckBox *m_tpm, *m_secureboot, *m_ram, *m_cpu, *m_storage, *m_applyAll;
    QCheckBox *m_createAccount, *m_admin, *m_skipMsAccount, *m_offline;
    QLineEdit *m_username;
    // Linux page (index 1)
    QWidget *m_linuxPage;
    QLabel *m_sbStatusLabel;
    QLabel *m_sbDetailLabel;
    // Placeholder page (index 2) — shown when no OS is detected
    QWidget *m_emptyPage;
};
#endif
