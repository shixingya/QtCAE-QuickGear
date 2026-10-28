#pragma once

#include "core/LicenseManager.h"

#include <QDialog>

class QLabel;
class QLineEdit;
class QPushButton;

/**
 * 激活对话框。
 *
 * 展示本机机器码（可一键复制）、当前授权状态，并支持导入离线授权文件。
 * 验签逻辑在 LicenseManager 中（HMAC-SHA256），本对话框只负责展示与导入。
 */
class ActivationDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ActivationDialog(LicenseManager *manager, QWidget *parent = nullptr);

signals:
    /** 授权状态发生变化（导入成功 / 失败），主窗口据此刷新状态栏 */
    void licenseChanged();

private slots:
    void copyMachineCode();
    void importLicenseFile();
    void openRepoPage();

private:
    void refresh();

    LicenseManager *m_manager = nullptr;
    QLineEdit      *m_code    = nullptr;
    QLabel         *m_status  = nullptr;
    QLabel         *m_detail  = nullptr;
    QPushButton    *m_copyBtn = nullptr;
};
