#include "MainWindow.h"
#include "WorkerThread.h"
#include "theme/Theme.h"
#include "sections/SourceSection.h"
#include "sections/TargetSection.h"
#include "sections/WindowsCustomizeSection.h"
#include "sections/AdvancedSection.h"
#include "ISOHashDialog.h"
#include "platform/linux/settings.h"
#include "platform/linux/wue.h"
#include <QApplication>
#include <QWidget>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QFrame>
#include <QStackedWidget>
#include <QButtonGroup>
#include <QPushButton>
#include <QLabel>
#include <QProgressBar>
#include <QElapsedTimer>
#include <QMessageBox>
#include <QFileInfo>

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
    settings_t *s = settings_open();
    m_dark = s ? settings_get_bool(s, "ui.dark", 1) : 1;
    if (s) settings_close(s);
    buildUi();
    winafi::applyTheme(qApp, m_dark);
    setWindowTitle("Winafi");
    resize(760, 540);
    onRefreshDevices();
}
MainWindow::~MainWindow() = default;

void MainWindow::buildUi() {
    auto *central = new QWidget(this);
    auto *outer = new QVBoxLayout(central); outer->setContentsMargins(0,0,0,0); outer->setSpacing(0);

    // Header
    auto *header = new QFrame(); auto *hl = new QHBoxLayout(header);
    auto *name = new QLabel("Winafi"); name->setStyleSheet("font-weight:800;font-size:15px;");
    m_statusChip = new QLabel(); setStatus(tr("Ready"), "#81c784");
    m_themeButton = new QPushButton(m_dark ? tr("Light") : tr("Dark")); m_themeButton->setObjectName("Ghost");
    hl->addWidget(name); hl->addStretch(1); hl->addWidget(m_statusChip); hl->addWidget(m_themeButton);
    outer->addWidget(header);

    // Body: nav + stack
    auto *body = new QHBoxLayout(); body->setContentsMargins(0,0,0,0);
    auto *nav = new QFrame(); nav->setObjectName("Nav"); nav->setFixedWidth(150);
    auto *navL = new QVBoxLayout(nav); navL->setSpacing(2);
    m_nav = new QButtonGroup(this);
    const QStringList items{tr("Source"), tr("Target"), tr("Options"), tr("Advanced")};
    for (int i = 0; i < items.size(); ++i) {
        auto *b = new QPushButton(items[i]); b->setObjectName("Nav"); b->setCheckable(true);
        if (i == 0) b->setChecked(true);
        m_nav->addButton(b, i); navL->addWidget(b);
    }
    navL->addStretch(1);

    m_stack = new QStackedWidget();
    m_source = new SourceSection(); m_target = new TargetSection();
    m_customize = new WindowsCustomizeSection(); m_advanced = new AdvancedSection();
    m_stack->addWidget(m_source); m_stack->addWidget(m_target);
    m_stack->addWidget(m_customize); m_stack->addWidget(m_advanced);

    body->addWidget(nav); body->addWidget(m_stack, 1);
    auto *bodyWrap = new QWidget(); bodyWrap->setLayout(body);
    outer->addWidget(bodyWrap, 1);

    // Footer
    auto *footer = new QFrame(); footer->setObjectName("Footer");
    auto *fl = new QVBoxLayout(footer);
    m_summaryLabel = new QLabel(tr("Select an ISO and a device to begin."));
    auto *progRow = new QHBoxLayout();
    m_progress = new QProgressBar(); m_progress->setValue(0);
    m_etaLabel = new QLabel(); m_etaLabel->setStyleSheet("color:#9d96b8;");
    m_startButton = new QPushButton(tr("START")); m_startButton->setEnabled(false);
    progRow->addWidget(m_progress, 1); progRow->addWidget(m_etaLabel); progRow->addWidget(m_startButton);
    fl->addWidget(m_summaryLabel); fl->addLayout(progRow);
    outer->addWidget(footer);

    setCentralWidget(central);

    connect(m_nav, QOverload<int>::of(&QButtonGroup::idClicked), this, &MainWindow::onNavChanged);
    connect(m_themeButton, &QPushButton::clicked, this, &MainWindow::onThemeToggled);
    connect(m_source, &SourceSection::isoChosen, this, &MainWindow::onIsoChosen);
    connect(m_source, &SourceSection::hashRequested, this, &MainWindow::onHashRequested);
    connect(m_source, &SourceSection::verifyRequested, this, &MainWindow::onVerifyRequested);
    connect(m_target, &TargetSection::refreshRequested, this, &MainWindow::onRefreshDevices);
    connect(m_target, &TargetSection::deviceChanged, this, &MainWindow::updateSummary);
    connect(m_customize, &WindowsCustomizeSection::changed, this, &MainWindow::updateSummary);
    connect(m_advanced, &AdvancedSection::changed, this, &MainWindow::updateSummary);
    connect(m_startButton, &QPushButton::clicked, this, &MainWindow::onStart);
}

void MainWindow::setStatus(const QString &text, const QString &colorHex) {
    m_statusChip->setText("● " + text);
    m_statusChip->setStyleSheet(QString("color:%1;font-weight:600;").arg(colorHex));
}
void MainWindow::onNavChanged(int index) { m_stack->setCurrentIndex(index); }

void MainWindow::onThemeToggled() {
    m_dark = !m_dark;
    winafi::applyTheme(qApp, m_dark);
    m_themeButton->setText(m_dark ? tr("Light") : tr("Dark"));
    settings_t *s = settings_open();
    if (s) { settings_set_bool(s, "ui.dark", m_dark ? 1 : 0); settings_close(s); }
}

void MainWindow::onRefreshDevices() {
    winafi_session_t *session = winafi_session_create();
    if (!session) return;
    winafi_device_t *devs = nullptr; int n = 0;
    m_target->clearDevices();
    if (winafi_enumerate_devices(session, &devs, &n) == WINAFI_OK) {
        for (int i = 0; i < n; ++i) {
            bool removable = devs[i].is_removable;
            if (!removable && !m_target->showHardDrives()) continue;
            QString label = QString("%1  (%2 GB)")
                .arg(QString::fromUtf8(devs[i].devnode))
                .arg(devs[i].capacity_bytes / (1024.0*1024.0*1024.0), 0, 'f', 1);
            m_target->addDevice(label, QString::fromUtf8(devs[i].devnode));
        }
    }
    winafi_session_destroy(session);
    updateSummary();
}

void MainWindow::onIsoChosen(const QString &path) {
    winafi_session_t *session = winafi_session_create();
    if (session && winafi_session_load_iso(session, path.toUtf8().constData()) == WINAFI_OK) {
        QString os = QString::fromUtf8(winafi_get_detected_os(session));
        bool isWin = os.contains("Windows", Qt::CaseInsensitive);
        bool isLinux = os.contains("Linux", Qt::CaseInsensitive);
        m_source->setOsBadge(os);
        if (isWin) {
            m_customize->setWindowsIso(true);
        } else if (isLinux) {
            int sbStatus = static_cast<int>(winafi_get_linux_sb_status(session));
            m_customize->setLinuxIso(sbStatus);
        } else {
            m_customize->setWindowsIso(false);
        }
    }
    if (session) winafi_session_destroy(session);
    updateSummary();
}

void MainWindow::onHashRequested() {
    const QString iso = m_source->isoPath();
    if (iso.isEmpty()) {
        QMessageBox::information(this, tr("No ISO"), tr("Select an ISO file first."));
        return;
    }
    // ISOHashDialog needs a session with the ISO loaded; it owns nothing, so we
    // keep the session alive for the dialog's modal lifetime then destroy it.
    winafi_session_t *session = winafi_session_create();
    if (session && winafi_session_load_iso(session, iso.toUtf8().constData()) == WINAFI_OK) {
        ISOHashDialog dlg(iso, session, this);
        dlg.exec();
    } else {
        QMessageBox::warning(this, tr("Cannot open ISO"), tr("Failed to load the selected ISO."));
    }
    if (session) winafi_session_destroy(session);
}

void MainWindow::onVerifyRequested() {
    // "Verify" re-runs source detection (OS type + badge) on the current ISO.
    const QString iso = m_source->isoPath();
    if (iso.isEmpty()) {
        QMessageBox::information(this, tr("No ISO"), tr("Select an ISO file first."));
        return;
    }
    onIsoChosen(iso);
}

void MainWindow::updateSummary() {
    const QString iso = m_source->isoPath();
    const QString dev = m_target->selectedDevnode();
    bool ready = !iso.isEmpty() && !dev.isEmpty();
    m_startButton->setEnabled(ready);
    if (!ready) { m_summaryLabel->setText(tr("Select an ISO and a device to begin.")); return; }
    QString bypass;
    int f = m_customize->wueFlags();
    if (f & WUE_BYPASS_TPM) bypass += "TPM ";
    if (f & WUE_BYPASS_SECUREBOOT) bypass += "SecureBoot ";
    if (f & WUE_BYPASS_RAM) bypass += "RAM ";
    if (f & WUE_BYPASS_CPU) bypass += "CPU ";
    if (f & WUE_BYPASS_STORAGE) bypass += "Storage ";
    m_summaryLabel->setText(QString("%1 → %2 · %3 · %4%5")
        .arg(QFileInfo(iso).fileName())
        .arg(dev)
        .arg(m_advanced->fileSystem())
        .arg(m_advanced->quickFormat() ? tr("quick") : tr("full"))
        .arg(bypass.isEmpty() ? QString() : " · bypass: " + bypass.trimmed()));
}

void MainWindow::onStart() {
    if (QMessageBox::warning(this, tr("Confirm"),
            tr("This will ERASE %1. Continue?").arg(m_target->selectedDevnode()),
            QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes)
        return;
    setStatus(tr("Writing…"), "#4fc3f7");
    m_startButton->setEnabled(false);
    m_timer.reset(new QElapsedTimer()); m_timer->start();
    m_worker.reset(new WorkerThread(
        m_source->isoPath(), m_target->selectedDevnode(),
        m_advanced->volumeLabel(), m_advanced->fileSystem(),
        m_advanced->clusterSize(), m_advanced->quickFormat(),
        static_cast<int>(WINAFI_IMAGE_STANDARD),
        m_customize->wueFlags(), m_customize->username()));
    connect(m_worker.get(), &WorkerThread::progressUpdated, this, &MainWindow::onProgress);
    connect(m_worker.get(), &WorkerThread::finished, this, &MainWindow::onFinished);
    m_worker->start();
}

void MainWindow::onProgress(int percent, QString message) {
    m_progress->setValue(percent);
    if (percent > 0 && m_timer) {
        qint64 elapsed = m_timer->elapsed();
        qint64 total = (qint64)(elapsed * 100.0 / percent);
        qint64 left = (total - elapsed) / 1000;
        m_etaLabel->setText(tr("~%1:%2 left").arg(left/60).arg(left%60, 2, 10, QChar('0')));
    }
    m_summaryLabel->setText(message);
}

void MainWindow::onFinished(bool ok, QString code, QString msg) {
    m_startButton->setEnabled(true);
    if (ok) { setStatus(tr("Done"), "#81c784"); m_progress->setValue(100); m_etaLabel->clear(); }
    else {
        setStatus(tr("Error"), "#d32f2f");
        m_summaryLabel->setText(tr("%1 — %2").arg(code, msg));
    }
}
