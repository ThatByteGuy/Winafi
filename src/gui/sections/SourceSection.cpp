#include "sections/SourceSection.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QLabel>
#include <QFileDialog>
#include <QUrl>

static QList<QUrl> mounted_drive_urls() {
    QList<QUrl> urls;
    static const char *skip_fstypes[] = {
        "proc", "sysfs", "tmpfs", "devtmpfs", "devpts",
        "cgroup", "cgroup2", "cpuset", "pstore",
        "securityfs", "selinuxfs", "autofs", "overlay",
        "aufs", "squashfs", "hugetlbfs", "mqueue", "bpf",
        "debugfs", "tracefs", "ramfs", "configfs",
        "efivarfs", "fusectl", "rpc_pipefs", "nsfs",
        "sys", "none", nullptr
    };
    FILE *fp = fopen("/proc/mounts", "r");
    if (!fp) return urls;
    char line[4096];
    while (fgets(line, sizeof(line), fp)) {
        char dev[256], mnt[256], fstype[64];
        if (sscanf(line, "%255s %255s %63s", dev, mnt, fstype) != 3)
            continue;
        bool skip = false;
        for (int i = 0; skip_fstypes[i]; i++) {
            if (strcmp(fstype, skip_fstypes[i]) == 0) { skip = true; break; }
        }
        if (skip || strcmp(dev, "none") == 0) continue;
        urls.append(QUrl::fromLocalFile(QString::fromUtf8(mnt)));
    }
    fclose(fp);
    return urls;
}

SourceSection::SourceSection(QWidget *parent) : QWidget(parent) {
    auto *root = new QVBoxLayout(this);
    auto *title = new QLabel(tr("Source image")); title->setStyleSheet("font-weight:600;");
    root->addWidget(title);

    auto *row = new QHBoxLayout();
    m_isoEdit = new QLineEdit(); m_isoEdit->setPlaceholderText(tr("Select a Windows or Linux ISO…"));
    m_browse  = new QPushButton(tr("Browse"));
    m_verify  = new QPushButton(tr("Verify")); m_verify->setObjectName("Ghost");
    m_hash    = new QPushButton(tr("Compute hash")); m_hash->setObjectName("Ghost");
    row->addWidget(m_isoEdit, 1); row->addWidget(m_browse); row->addWidget(m_verify); row->addWidget(m_hash);
    root->addLayout(row);

    m_osBadge = new QLabel(); m_osBadge->setStyleSheet("color:#9d96b8;");
    m_hashLabel = new QLabel(); m_hashLabel->setStyleSheet("color:#9d96b8;font-family:monospace;");
    root->addWidget(m_osBadge);
    root->addWidget(m_hashLabel);
    root->addStretch(1);

    connect(m_browse, &QPushButton::clicked, this, [this]{
        QFileDialog dialog(this, tr("Select ISO"), QString(), tr("ISO images (*.iso)"));
        dialog.setFileMode(QFileDialog::ExistingFile);
        dialog.setOption(QFileDialog::DontUseNativeDialog);

        QList<QUrl> sidebar = mounted_drive_urls();
        if (!sidebar.isEmpty())
            dialog.setSidebarUrls(sidebar);

        if (dialog.exec() == QDialog::Accepted) {
            QStringList files = dialog.selectedFiles();
            if (!files.isEmpty()) {
                m_isoEdit->setText(files.first());
                emit isoChosen(files.first());
            }
        }
    });
    connect(m_verify, &QPushButton::clicked, this, &SourceSection::verifyRequested);
    connect(m_hash,   &QPushButton::clicked, this, &SourceSection::hashRequested);
}
QString SourceSection::isoPath() const { return m_isoEdit->text(); }
void SourceSection::setOsBadge(const QString &t) { m_osBadge->setText(t.isEmpty()? QString() : tr("Detected: %1").arg(t)); }
void SourceSection::setHash(const QString &h) { m_hashLabel->setText(h.isEmpty()? QString() : "SHA-256: " + h); }
