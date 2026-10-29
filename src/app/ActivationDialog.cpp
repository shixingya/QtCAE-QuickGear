#include "ActivationDialog.h"

#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QFileDialog>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

namespace {
const char *kRepoUrl = "https://github.com/shixingya/QtCAE-QuickGear";
}

ActivationDialog::ActivationDialog(LicenseManager *manager, QWidget *parent)
    : QDialog(parent)
    , m_manager(manager)
{
    setWindowTitle(QStringLiteral("激活 / 授权"));
    setMinimumWidth(520);

    auto *title = new QLabel(QStringLiteral("本机机器码"), this);
    title->setObjectName(QStringLiteral("sectionTitle"));

    m_code = new QLineEdit(LicenseManager::machineCode(), this);
    m_code->setReadOnly(true);
    m_code->setCursorPosition(0);
    m_code->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));

    m_copyBtn = new QPushButton(QStringLiteral("复制"), this);
    m_copyBtn->setObjectName(QStringLiteral("ghostButton"));
    connect(m_copyBtn, &QPushButton::clicked, this, &ActivationDialog::copyMachineCode);

    auto *codeRow = new QHBoxLayout;
    codeRow->addWidget(m_code, 1);
    codeRow->addWidget(m_copyBtn);

    auto *hint = new QLabel(
        QStringLiteral("机器码由本机硬件信息派生，换主板或重装系统会变化。\n"
                       "网页端展示的是浏览器设备码，仅用于沟通定位；激活以客户端机器码为准。"),
        this);
    hint->setObjectName(QStringLiteral("hintLabel"));
    hint->setWordWrap(true);

    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("sectionTitle"));

    m_detail = new QLabel(this);
    m_detail->setObjectName(QStringLiteral("hintLabel"));
    m_detail->setWordWrap(true);
    m_detail->setTextInteractionFlags(Qt::TextSelectableByMouse);

    auto *importBtn = new QPushButton(QStringLiteral("导入授权文件…"), this);
    importBtn->setObjectName(QStringLiteral("primaryButton"));
    connect(importBtn, &QPushButton::clicked, this, &ActivationDialog::importLicenseFile);

    auto *repoBtn = new QPushButton(QStringLiteral("前往仓库"), this);
    repoBtn->setObjectName(QStringLiteral("ghostButton"));
    connect(repoBtn, &QPushButton::clicked, this, &ActivationDialog::openRepoPage);

    auto *closeBtn = new QPushButton(QStringLiteral("关闭"), this);
    closeBtn->setObjectName(QStringLiteral("ghostButton"));
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);

    auto *buttonRow = new QHBoxLayout;
    buttonRow->addWidget(importBtn);
    buttonRow->addWidget(repoBtn);
    buttonRow->addStretch(1);
    buttonRow->addWidget(closeBtn);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(20, 20, 20, 16);
    root->setSpacing(10);
    root->addWidget(title);
    root->addLayout(codeRow);
    root->addWidget(hint);
    root->addSpacing(6);
    root->addWidget(m_status);
    root->addWidget(m_detail);
    root->addSpacing(6);
    root->addLayout(buttonRow);

    refresh();
}

void ActivationDialog::refresh()
{
    const LicenseInfo info = m_manager->current();

    if (info.valid) {
        m_status->setText(QStringLiteral("授权状态：已激活（%1）").arg(info.edition));

        QString detail = QStringLiteral("授权对象：%1")
                             .arg(info.licensee.isEmpty() ? QStringLiteral("—") : info.licensee);
        if (info.expire.isValid())
            detail += QStringLiteral("\n有效期至：%1（%2）")
                          .arg(info.expire.toString(QStringLiteral("yyyy-MM-dd")),
                               LicenseManager::expiryText(info));
        detail += QStringLiteral("\n签名：%1").arg(LicenseManager::algName(info.alg));
        detail += QStringLiteral("\n%1").arg(info.message);
        if (info.expiringSoon())
            detail += QStringLiteral("\n续期：把上面的机器码发给作者，收到新的 .qglic 后重新导入。");
        m_detail->setText(detail);
        m_detail->setStyleSheet(info.expiringSoon() ? QStringLiteral("color:#B4692F;")
                                                    : QString());
    } else {
        const bool expired = info.state == LicenseState::Expired;
        m_status->setText(expired ? QStringLiteral("授权状态：已过期")
                                  : QStringLiteral("授权状态：未激活（试用）"));

        QString detail = info.message;
        if (expired)
            detail += QStringLiteral("\n续期：把上面的机器码发给作者，收到新的 .qglic 后重新导入。");
        detail += QStringLiteral("\n授权文件路径：%1").arg(LicenseManager::defaultLicensePath());
        m_detail->setText(detail);
        m_detail->setStyleSheet(expired ? QStringLiteral("color:#DC2626;") : QString());
    }
}

void ActivationDialog::copyMachineCode()
{
    QApplication::clipboard()->setText(m_code->text());
    const QString old = m_copyBtn->text();
    m_copyBtn->setText(QStringLiteral("已复制"));
    m_copyBtn->setEnabled(false);
    QTimer::singleShot(1500, this, [this, old] {
        m_copyBtn->setText(old);
        m_copyBtn->setEnabled(true);
    });
}

void ActivationDialog::importLicenseFile()
{
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("选择授权文件"), QString(),
        QStringLiteral("QuickGear 授权 (*.qglic);;所有文件 (*.*)"));
    if (path.isEmpty())
        return;

    QString error;
    if (!m_manager->installFrom(path, &error)) {
        QMessageBox::warning(this, QStringLiteral("导入失败"), error);
        return;
    }

    const LicenseInfo info = m_manager->current();
    refresh();
    emit licenseChanged();

    if (info.valid) {
        // installFrom 成功时也可能带话，丢掉它等于让客户以为一切如预期。
        // （比如客户以为手上那张长期授权还在，实际已经被新文件换掉了）
        const QString extra = error.isEmpty() ? QString() : QStringLiteral("\n%1").arg(error);
        QMessageBox::information(this, QStringLiteral("导入成功"),
                                 QStringLiteral("授权已生效：%1%2").arg(info.message, extra));
    } else {
        // 走到这里说明文件已经换进默认路径了，只是用不了（最典型：已过期）。
        // 标题写“已替换”而不是“不可用”，否则客户会以为导入失败，回头再试一遍
        const QString extra = error.isEmpty() ? QString() : QStringLiteral("\n%1").arg(error);
        QMessageBox::warning(this, QStringLiteral("授权文件已替换，但当前不可用"),
                             QStringLiteral("%1%2").arg(info.message, extra));
    }
}

void ActivationDialog::openRepoPage()
{
    QDesktopServices::openUrl(QUrl(QString::fromLatin1(kRepoUrl)));
}
