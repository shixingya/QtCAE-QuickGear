#pragma once

#include "core/ISolverBackend.h"
#include "core/LicenseManager.h"
#include "core/ProjectFile.h"
#include "core/StlMesh.h"

#include <QImage>
#include <QMainWindow>
#include <QStringList>

#include <memory>

class QAction;
class QCloseEvent;
class QDockWidget;
class QDragEnterEvent;
class QDropEvent;
class QLabel;
class QLineEdit;
class QMenu;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QThread;

class ActivationDialog;
class ParamPanel;
class StressView;

/**
 * 主窗口：左参数 / 中云图 / 下指标 + 日志 + 状态栏。
 *
 * 围绕一条完整的工作闭环组织：
 *   导入几何(STL) → 求解 → 看云图 → 导出结论(PDF/PNG) → 存工程(.qgpj)
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

protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void closeEvent(QCloseEvent *event) override;

private slots:
    void runSolve();
    void onSolveProgress(int percent, const QString &stage);
    void onSolveFinished(const SolveResult &result);
    void onParamsChanged();
    void openActivationDialog();
    void showAbout();

    void newProject();
    void openProject();
    void saveProject();
    void saveProjectAs();
    void importStl();
    void exportPdf();
    void exportPng();
    void openRecent();

private:
    void buildUi();
    void buildMenus();
    void buildLogDock();

    void refreshBackendBadge();
    void refreshLicenseBadge();
    void remindLicenseExpiry();
    void refreshActions();
    void updateWindowTitle();
    void setBusy(bool busy);
    void resetMetrics();
    void logLine(const QString &text);

    bool saveTo(const QString &path);
    bool loadFrom(const QString &path);
    bool importStlFrom(const QString &path);
    bool maybeSaveChanges();
    void applyProject(const ProjectData &data);
    void rememberRecent(const QString &path);
    void restoreSettings();
    void storeSettings();
    QString defaultExportDir() const;
    QImage  renderStressImage() const;

    QString buildReportContextAndExport(bool pdf);

    // 用 shared_ptr 而不是 unique_ptr：求解线程里的 worker 会一起持有它，
    // 关窗口时窗口先没、后端还在，worker 就不会踩到已经释放的求解器
    std::shared_ptr<ISolverBackend> m_backend;
    LicenseManager                  m_license;
    LicenseInfo                     m_licenseInfo;   // 启动时读一次，徽标与到期提醒共用

    ParamPanel   *m_paramPanel  = nullptr;
    StressView   *m_stressView  = nullptr;
    QPushButton  *m_runButton   = nullptr;
    QProgressBar *m_progress    = nullptr;
    QLineEdit    *m_noteEdit    = nullptr;
    QPlainTextEdit *m_log       = nullptr;
    QDockWidget  *m_logDock     = nullptr;

    QLabel *m_sigmaLabel = nullptr;
    QLabel *m_dispLabel  = nullptr;
    QLabel *m_meshLabel  = nullptr;
    QLabel *m_safeLabel  = nullptr;

    QLabel *m_stageLabel   = nullptr;   // 状态栏：阶段 / 后端
    QLabel *m_licenseLabel = nullptr;   // 状态栏：授权状态
    QLabel *m_geometryLabel = nullptr;  // 状态栏：几何来源

    QAction *m_saveAction        = nullptr;
    QAction *m_exportPdfAction   = nullptr;
    QAction *m_exportPngAction   = nullptr;
    QMenu   *m_recentMenu        = nullptr;

    QThread *m_thread = nullptr;
    bool     m_busy   = false;
    bool     m_closeWhenIdle = false;   // 求解中点了关闭：等算完再关，不在半路拆线程

    QString    m_projectPath;      // 当前工程文件路径，空表示未保存
    QStringList m_recentFiles;
    StlMesh    m_mesh;             // 导入几何（可为空）
    SolveResult m_lastResult;
    bool       m_hasResult = false;
    bool       m_dirty     = false;
};
