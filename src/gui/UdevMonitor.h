#ifndef UDEVMONITOR_H
#define UDEVMONITOR_H
#include <QObject>
#include <QSocketNotifier>
#include <QTimer>
#include <libudev.h>

class UdevMonitor : public QObject {
    Q_OBJECT
public:
    explicit UdevMonitor(QObject *parent = nullptr);
    ~UdevMonitor();

signals:
    void devicesChanged();

private slots:
    void onUdevActivity();
    void onDebounceTimeout();

private:
    struct udev *m_udev;
    struct udev_monitor *m_mon;
    QSocketNotifier *m_notifier;
    QTimer *m_debounce;
};

#endif
