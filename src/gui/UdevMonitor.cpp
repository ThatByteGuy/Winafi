#include "UdevMonitor.h"
#include <QDebug>

UdevMonitor::UdevMonitor(QObject *parent)
    : QObject(parent), m_udev(nullptr), m_mon(nullptr), m_notifier(nullptr), m_debounce(nullptr) {
    m_udev = udev_new();
    if (!m_udev) {
        qWarning("UdevMonitor: failed to create udev context");
        return;
    }

    m_mon = udev_monitor_new_from_netlink(m_udev, "udev");
    if (!m_mon) {
        qWarning("UdevMonitor: failed to create udev monitor");
        udev_unref(m_udev);
        m_udev = nullptr;
        return;
    }

    udev_monitor_filter_add_match_subsystem_devtype(m_mon, "block", NULL);
    udev_monitor_enable_receiving(m_mon);

    int fd = udev_monitor_get_fd(m_mon);
    if (fd < 0) {
        qWarning("UdevMonitor: failed to get monitor fd");
        udev_monitor_unref(m_mon);
        m_mon = nullptr;
        udev_unref(m_udev);
        m_udev = nullptr;
        return;
    }

    m_notifier = new QSocketNotifier(fd, QSocketNotifier::Read, this);
    connect(m_notifier, &QSocketNotifier::activated, this, &UdevMonitor::onUdevActivity);

    m_debounce = new QTimer(this);
    m_debounce->setSingleShot(true);
    m_debounce->setInterval(500);
    connect(m_debounce, &QTimer::timeout, this, &UdevMonitor::onDebounceTimeout);
}

UdevMonitor::~UdevMonitor() {
    delete m_notifier;
    delete m_debounce;
    if (m_mon) udev_monitor_unref(m_mon);
    if (m_udev) udev_unref(m_udev);
}

void UdevMonitor::onUdevActivity() {
    struct udev_device *dev = udev_monitor_receive_device(m_mon);
    if (!dev) return;

    const char *action = udev_device_get_action(dev);
    if (action) {
        const char *devtype = udev_device_get_devtype(dev);
        // Only react to whole-disk events (not partitions)
        // Also catch remove events which may not have devtype
        if (!devtype || strcmp(devtype, "disk") == 0) {
            m_debounce->start();
        }
    }
    udev_device_unref(dev);
}

void UdevMonitor::onDebounceTimeout() {
    emit devicesChanged();
}
