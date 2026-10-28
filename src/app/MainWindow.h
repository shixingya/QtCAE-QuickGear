#pragma once

#include "core/ISolverBackend.h"
#include "core/LicenseManager.h"

#include <QMainWindow>

#include <memory>

class QAction;
class QLabel;
class QProgressBar;
class QPushButton;
class QThread;

class ActivationDialog;
class ParamPanel;
class StressView;

/**
 * 主窗口：左参数 / 中云图 / 下指标 + 状态栏。
 *
 * 与闭源求解器的接触面只有 ISolverBackend 一个接口，
 * 因此本类可以在没有加密库的情况下完整编译运行。
 */
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

private slots:
    void runSolve();
    void onSolveProgress(int percent, const QString &stage);
    void onSolveFinished(const SolveResult &result);
    void onParamsChanged();
    void openActivationDialog();
    void showAbout();

private:
    void buildUi();
    void buildMenus();
    void refreshBackendBadge();
    void refreshLicenseBadge();
    void setBusy(bool busy);
    void resetMetrics();

    std::unique_ptr<ISolverBackend> m_backend;
    LicenseManager                  m_license;

    ParamPanel   *m_paramPanel  = nullptr;
    StressView   *m_stressView  = nullptr;
    QPushButton  *m_runButton   = nullptr;
    QProgressBar *m_progress    = nullptr;

    QLabel *m_sigmaLabel = nullptr;
    QLabel *m_dispLabel  = nullptr;
    QLabel *m_meshLabel  = nullptr;
    QLabel *m_safeLabel  = nullptr;

    QLabel *m_stageLabel  = nullptr;   // 状态栏：阶段 / 后端
    QLabel *m_licenseLabel = nullptr;  // 状态栏：授权状态

    QThread *m_thread = nullptr;
    bool     m_busy   = false;
};
