#ifndef MAINWINDOW_H
#define MAINWINDOW_H
#include "winafi.h"
#include "platform/linux/iso_extract.h"
#include <QMainWindow>
#include <memory>
#include <QMap>
class QStackedWidget; class QLabel; class QProgressBar; class QPushButton;
class QButtonGroup; class QElapsedTimer;
class SourceSection; class TargetSection; class WindowsCustomizeSection; class AdvancedSection;
class WorkerThread; class UdevMonitor;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

    struct DeviceInfo {
        QString devnode;
        QString vendor;
        QString model;
        QString mountPoint;
        uint64_t capacityBytes;
        bool isRemovable;
        bool isMounted;
    };

private slots:
    void onNavChanged(int index);
    void onThemeToggled();
    void onRefreshDevices();
    void onIsoChosen(const QString &path);
    void onHashRequested();
    void onVerifyRequested();
    void onStart();
    void onProgress(int percent, QString message);
    void onFinished(bool ok, QString code, QString msg);
    void updateSummary();
    void updateDeviceInfo();
private:
    void buildUi();
    void setStatus(const QString &text, const QString &colorHex);
    QString formatDeviceLabel(const DeviceInfo &info) const;
    QStackedWidget *m_stack;
    SourceSection *m_source; TargetSection *m_target;
    WindowsCustomizeSection *m_customize; AdvancedSection *m_advanced;
    QLabel *m_statusChip; QLabel *m_summaryLabel; QLabel *m_etaLabel;
    QProgressBar *m_progress; QPushButton *m_startButton; QPushButton *m_themeButton;
    QButtonGroup *m_nav;
    std::unique_ptr<WorkerThread> m_worker;
    std::unique_ptr<QElapsedTimer> m_timer;
    bool m_dark = true;

    QMap<QString, DeviceInfo> m_deviceInfo;
    std::unique_ptr<UdevMonitor> m_udevMonitor;
};
#endif
