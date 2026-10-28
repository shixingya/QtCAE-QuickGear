#include "MainWindow.h"

#include "ActivationDialog.h"
#include "ParamPanel.h"
#include "ReportBuilder.h"
#include "SolveWorker.h"
#include "StressView.h"
#include "core/SolverFactory.h"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QDockWidget>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QStatusBar>
#include <QThread>
#include <QTime>
#include <QUrl>
#include <QVBoxLayout>

namespace {

const char *kRepoUrl    = "https://github.com/shixingya/QtCAE-QuickGear";
const char *kWebsiteUrl = "https://shixingya.github.io/QtCAE-QuickGear/";

/** 一个指标块：标题 + 数值 */
QWidget *makeMetric(const QString &title, QLabel **valueOut, QWidget *parent)
{
    auto *box = new QWidget(parent);
    box->setObjectName(QStringLiteral("metricBox"));

    auto *titleLabel = new QLabel(title, box);
    titleLabel->setObjectName(QStringLiteral("metricTitle"));

    auto *valueLabel = new QLabel(QStringLiteral("--"), box);
    valueLabel->setObjectName(QStringLiteral("metricValue"));

    auto *layout = new QVBoxLayout(box);
    layout->setContentsMargins(10, 8, 10, 8);
    layout->setSpacing(2);
    layout->addWidget(titleLabel);
    layout->addWidget(valueLabel);

    *valueOut = valueLabel;
    return box;
}

bool hasSuffix(const QString &path, const char *suffix)
{
    return path.endsWith(QLatin1String(suffix), Qt::CaseInsensitive);
}

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    m_backend.reset(createSolverBackend());

    buildUi();
    buildMenus();
    buildLogDock();

    setAcceptDrops(true);

    refreshLicenseBadge();
    resetMetrics();
    restoreSettings();

    m_stressView->setDemoMode(m_backend && m_backend->isDemo());
    refreshBackendBadge();
    refreshActions();
    updateWindowTitle();

    logLine(QStringLiteral("启动完成 · 后端 %1")
                .arg(m_backend ? m_backend->displayName() : QStringLiteral("未配置后端")));
    logLine(QStringLiteral("本机机器码 %1").arg(LicenseManager::machineCode()));
    logLine(QStringLiteral("工作目录 %1")
                .arg(QDir::toNativeSeparators(QStandardPaths::writableLocation(
                    QStandardPaths::AppDataLocation))));
}

MainWindow::~MainWindow()
{
    if (m_thread) {
        m_thread->quit();
        m_thread->wait(3000);
    }
}

void MainWindow::buildUi()
{
    auto *central = new QWidget(this);
    central->setObjectName(QStringLiteral("centralArea"));

    // ---- 左：参数面板 + 运行按钮 + 工况备注 ----
    m_paramPanel = new ParamPanel(central);
    m_paramPanel->setFixedWidth(288);

    m_runButton = new QPushButton(QStringLiteral("运行仿真"), central);
    m_runButton->setObjectName(QStringLiteral("runButton"));
    m_runButton->setMinimumHeight(38);
    m_runButton->setCursor(Qt::PointingHandCursor);
    connect(m_runButton, &QPushButton::clicked, this, &MainWindow::runSolve);
    connect(m_paramPanel, &ParamPanel::paramsChanged, this, &MainWindow::onParamsChanged);

    auto *noteLabel = new QLabel(QStringLiteral("工况备注"), central);
    noteLabel->setObjectName(QStringLiteral("sectionTitle"));

    m_noteEdit = new QLineEdit(central);
    m_noteEdit->setPlaceholderText(QStringLiteral("例如：一级减速器，额定工况"));
    m_noteEdit->setMaxLength(120);
    connect(m_noteEdit, &QLineEdit::textEdited, this, [this] {
        m_dirty = true;
        updateWindowTitle();
    });

    auto *leftColumn = new QVBoxLayout;
    leftColumn->setContentsMargins(0, 0, 0, 0);
    leftColumn->setSpacing(12);
    leftColumn->addWidget(m_paramPanel);
    leftColumn->addWidget(m_runButton);
    leftColumn->addWidget(noteLabel);
    leftColumn->addWidget(m_noteEdit);
    leftColumn->addStretch(1);

    auto *leftWrap = new QWidget(central);
    leftWrap->setLayout(leftColumn);
    leftWrap->setFixedWidth(288);

    // ---- 右：云图 + 指标 + 进度 ----
    m_stressView = new StressView(central);

    auto *metricRow = new QHBoxLayout;
    metricRow->setContentsMargins(0, 0, 0, 0);
    metricRow->setSpacing(10);
    metricRow->addWidget(makeMetric(QStringLiteral("峰值等效应力 σv (MPa)"), &m_sigmaLabel, central));
    metricRow->addWidget(makeMetric(QStringLiteral("最大位移 (μm)"), &m_dispLabel, central));
    metricRow->addWidget(makeMetric(QStringLiteral("网格单元 (个)"), &m_meshLabel, central));
    metricRow->addWidget(makeMetric(QStringLiteral("安全系数 n"), &m_safeLabel, central));

    m_progress = new QProgressBar(central);
    m_progress->setRange(0, 100);
    m_progress->setTextVisible(true);
    m_progress->setVisible(false);

    auto *rightColumn = new QVBoxLayout;
    rightColumn->setContentsMargins(0, 0, 0, 0);
    rightColumn->setSpacing(10);
    rightColumn->addWidget(m_stressView, 1);
    rightColumn->addLayout(metricRow);
    rightColumn->addWidget(m_progress);

    auto *rootLayout = new QHBoxLayout(central);
    rootLayout->setContentsMargins(16, 16, 16, 12);
    rootLayout->setSpacing(16);
    rootLayout->addWidget(leftWrap);
    rootLayout->addLayout(rightColumn, 1);

    setCentralWidget(central);
    resize(1120, 760);

    // ---- 状态栏 ----
    m_stageLabel    = new QLabel(this);
    m_geometryLabel = new QLabel(this);
    m_licenseLabel  = new QLabel(this);
    m_licenseLabel->setObjectName(QStringLiteral("licenseBadge"));
    statusBar()->addWidget(m_stageLabel, 1);
    statusBar()->addPermanentWidget(m_geometryLabel);
    statusBar()->addPermanentWidget(m_licenseLabel);
}

void MainWindow::buildMenus()
{
    auto *fileMenu = menuBar()->addMenu(QStringLiteral("文件(&F)"));

    auto *newAction = fileMenu->addAction(QStringLiteral("新建工程"));
    newAction->setShortcut(QKeySequence::New);
    connect(newAction, &QAction::triggered, this, &MainWindow::newProject);

    auto *openAction = fileMenu->addAction(QStringLiteral("打开工程…"));
    openAction->setShortcut(QKeySequence::Open);
    connect(openAction, &QAction::triggered, this, &MainWindow::openProject);

    m_recentMenu = fileMenu->addMenu(QStringLiteral("最近打开"));
    connect(m_recentMenu, &QMenu::aboutToShow, this, [this] {
        m_recentMenu->clear();
        if (m_recentFiles.isEmpty()) {
            auto *empty = m_recentMenu->addAction(QStringLiteral("（暂无）"));
            empty->setEnabled(false);
            return;
        }
        for (const QString &path : m_recentFiles) {
            auto *action = m_recentMenu->addAction(QFileInfo(path).fileName());
            action->setToolTip(QDir::toNativeSeparators(path));
            action->setData(path);
            connect(action, &QAction::triggered, this, &MainWindow::openRecent);
        }
    });

    m_saveAction = fileMenu->addAction(QStringLiteral("保存"));
    m_saveAction->setShortcut(QKeySequence::Save);
    connect(m_saveAction, &QAction::triggered, this, &MainWindow::saveProject);

    auto *saveAsAction = fileMenu->addAction(QStringLiteral("另存为…"));
    saveAsAction->setShortcut(QKeySequence::SaveAs);
    connect(saveAsAction, &QAction::triggered, this, &MainWindow::saveProjectAs);

    fileMenu->addSeparator();

    auto *importAction = fileMenu->addAction(QStringLiteral("导入 STL 几何…"));
    importAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+I")));
    connect(importAction, &QAction::triggered, this, &MainWindow::importStl);

    fileMenu->addSeparator();

    m_exportPdfAction = fileMenu->addAction(QStringLiteral("导出结论 PDF…"));
    m_exportPdfAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+E")));
    connect(m_exportPdfAction, &QAction::triggered, this, &MainWindow::exportPdf);

    m_exportPngAction = fileMenu->addAction(QStringLiteral("导出云图 PNG…"));
    m_exportPngAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+E")));
    connect(m_exportPngAction, &QAction::triggered, this, &MainWindow::exportPng);

    fileMenu->addSeparator();

    auto *quitAction = fileMenu->addAction(QStringLiteral("退出"));
    quitAction->setShortcut(QKeySequence::Quit);
    connect(quitAction, &QAction::triggered, this, &QWidget::close);

    auto *solveMenu = menuBar()->addMenu(QStringLiteral("仿真(&S)"));
    auto *runAction = solveMenu->addAction(QStringLiteral("运行仿真"));
    runAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+R")));
    connect(runAction, &QAction::triggered, this, &MainWindow::runSolve);

    auto *licenseMenu = menuBar()->addMenu(QStringLiteral("授权(&L)"));
    auto *activateAction = licenseMenu->addAction(QStringLiteral("激活 / 查看机器码…"));
    connect(activateAction, &QAction::triggered, this, &MainWindow::openActivationDialog);

    auto *viewMenu = menuBar()->addMenu(QStringLiteral("视图(&V)"));
    auto *logAction = viewMenu->addAction(QStringLiteral("求解日志"));
    logAction->setCheckable(true);
    logAction->setChecked(true);
    connect(logAction, &QAction::toggled, this, [this](bool visible) {
        if (m_logDock)
            m_logDock->setVisible(visible);
    });

    auto *helpMenu = menuBar()->addMenu(QStringLiteral("帮助(&H)"));
    auto *siteAction = helpMenu->addAction(QStringLiteral("打开在线演示…"));
    connect(siteAction, &QAction::triggered, this, [] {
        QDesktopServices::openUrl(QUrl(QString::fromLatin1(kWebsiteUrl)));
    });
    auto *repoAction = helpMenu->addAction(QStringLiteral("打开 GitHub 仓库"));
    connect(repoAction, &QAction::triggered, this, [] {
        QDesktopServices::openUrl(QUrl(QString::fromLatin1(kRepoUrl)));
    });
    auto *aboutAction = helpMenu->addAction(QStringLiteral("关于"));
    connect(aboutAction, &QAction::triggered, this, &MainWindow::showAbout);
}

void MainWindow::buildLogDock()
{
    m_logDock = new QDockWidget(QStringLiteral("求解日志"), this);
    m_logDock->setObjectName(QStringLiteral("logDock"));
    m_logDock->setAllowedAreas(Qt::BottomDockWidgetArea | Qt::TopDockWidgetArea);

    m_log = new QPlainTextEdit(m_logDock);
    m_log->setObjectName(QStringLiteral("logView"));
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(2000);
    QFont mono(QStringLiteral("Consolas"));
    mono.setPointSizeF(9.0);
    m_log->setFont(mono);

    m_logDock->setWidget(m_log);
    addDockWidget(Qt::BottomDockWidgetArea, m_logDock);
    resizeDocks({m_logDock}, {104}, Qt::Vertical);
}

void MainWindow::refreshBackendBadge()
{
    const QString backendName = m_backend ? m_backend->displayName() : QStringLiteral("未配置后端");
    const bool demo = m_backend && m_backend->isDemo();

    m_stageLabel->setText(demo ? QStringLiteral("就绪 · %1（结果为示意数据）").arg(backendName)
                               : QStringLiteral("就绪 · %1").arg(backendName));
    updateWindowTitle();
}

void MainWindow::updateWindowTitle()
{
    const QString backendName = m_backend ? m_backend->displayName() : QStringLiteral("未配置后端");
    const QString name = m_projectPath.isEmpty() ? QStringLiteral("未命名工程")
                                                 : QFileInfo(m_projectPath).fileName();
    setWindowTitle(QStringLiteral("%1%2 · %3 · QtCAE-QuickGear %4")
                       .arg(name, m_dirty ? QStringLiteral(" *") : QString(),
                            backendName, QCoreApplication::applicationVersion()));
}

void MainWindow::refreshLicenseBadge()
{
    const LicenseInfo info = m_license.current();
    if (info.valid) {
        m_licenseLabel->setText(QStringLiteral("授权：已激活（%1）").arg(info.edition));
        m_licenseLabel->setStyleSheet(QStringLiteral("color:#059669;"));
    } else {
        m_licenseLabel->setText(QStringLiteral("授权：未激活（试用版）"));
        m_licenseLabel->setStyleSheet(QStringLiteral("color:#B4692F;"));
    }
    m_licenseLabel->setToolTip(info.message);
}

void MainWindow::refreshActions()
{
    if (m_exportPdfAction)
        m_exportPdfAction->setEnabled(m_hasResult && !m_busy);
    if (m_exportPngAction)
        m_exportPngAction->setEnabled(m_hasResult && !m_busy);
    if (m_saveAction)
        m_saveAction->setEnabled(!m_busy);

    if (m_geometryLabel) {
        m_geometryLabel->setText(
            m_mesh.isEmpty()
                ? QStringLiteral("几何：参数化齿轮")
                : QStringLiteral("几何：%1（%2 面）")
                      .arg(QFileInfo(m_mesh.sourcePath).fileName())
                      .arg(QLocale().toString(m_mesh.count())));
        m_geometryLabel->setToolTip(m_mesh.isEmpty() ? QString() : stlSummary(m_mesh));
    }
}

void MainWindow::resetMetrics()
{
    const QString placeholder = QStringLiteral("--");
    m_sigmaLabel->setText(placeholder);
    m_dispLabel->setText(placeholder);
    m_meshLabel->setText(placeholder);
    m_safeLabel->setText(placeholder);
}

void MainWindow::setBusy(bool busy)
{
    m_busy = busy;
    m_runButton->setEnabled(!busy);
    m_runButton->setText(busy ? QStringLiteral("求解中…") : QStringLiteral("运行仿真"));
    m_paramPanel->setEditable(!busy);
    m_progress->setVisible(busy);
    if (!busy)
        m_progress->setValue(0);
    refreshActions();
}

void MainWindow::logLine(const QString &text)
{
    if (!m_log)
        return;
    m_log->appendPlainText(QStringLiteral("[%1] %2")
                               .arg(QTime::currentTime().toString(QStringLiteral("HH:mm:ss")), text));
}

void MainWindow::onParamsChanged()
{
    const GearParams params = m_paramPanel->params();
    m_stressView->setGear(params.teeth, params.pinionTeeth, params.module);
    m_stressView->clearResult();
    m_lastResult = SolveResult();
    m_hasResult  = false;
    resetMetrics();
    m_dirty = true;
    refreshActions();
    updateWindowTitle();
    if (!m_busy)
        m_stageLabel->setText(QStringLiteral("参数已变更，待求解"));
}

void MainWindow::runSolve()
{
    if (m_busy || !m_backend)
        return;

    setBusy(true);
    m_progress->setValue(0);
    m_stageLabel->setText(QStringLiteral("准备求解…"));

    const GearParams params = m_paramPanel->params();
    logLine(QStringLiteral("开始求解 · z=%1/%2 · m=%3 · T=%4 N·m · b=%5 mm · %6")
                .arg(params.teeth)
                .arg(params.pinionTeeth)
                .arg(params.module, 0, 'f', 1)
                .arg(params.torque, 0, 'f', 0)
                .arg(params.faceWidth, 0, 'f', 1)
                .arg(params.material));

    auto *thread = new QThread;
    auto *worker = new SolveWorker(m_backend.get(), params);
    worker->moveToThread(thread);

    connect(thread, &QThread::started, worker, &SolveWorker::run);
    connect(worker, &SolveWorker::progress, this, &MainWindow::onSolveProgress);
    connect(worker, &SolveWorker::finished, this, &MainWindow::onSolveFinished);
    connect(worker, &SolveWorker::finished, thread, &QThread::quit);
    connect(worker, &SolveWorker::finished, worker, &QObject::deleteLater);
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    connect(thread, &QThread::finished, this, [this] { m_thread = nullptr; });

    m_thread = thread;
    thread->start();
}

void MainWindow::onSolveProgress(int percent, const QString &stage)
{
    m_progress->setValue(percent);
    m_stageLabel->setText(stage);
    logLine(QStringLiteral("%1% · %2").arg(percent).arg(stage));
}

void MainWindow::onSolveFinished(const SolveResult &result)
{
    setBusy(false);

    if (!result.ok) {
        m_stageLabel->setText(QStringLiteral("求解失败"));
        logLine(QStringLiteral("求解失败：%1").arg(result.error));
        QMessageBox::warning(this, QStringLiteral("求解失败"), result.error);
        return;
    }

    m_lastResult = result;
    m_hasResult  = true;
    m_dirty      = true;

    m_stressView->setResult(result);

    m_sigmaLabel->setText(QString::number(result.peakVonMises, 'f', 1));
    m_dispLabel->setText(QString::number(result.maxDeflection, 'f', 1));
    m_meshLabel->setText(QLocale().toString(result.elements));
    m_safeLabel->setText(QString::number(result.safetyFactor, 'f', 2));

    bool passed = false;
    const QString verdict = conclusionText(result, &passed);
    m_stageLabel->setText(QStringLiteral("求解完成 · %1 单元 / %2 节点 · 耗时 %3 s")
                              .arg(QLocale().toString(result.elements),
                                   QLocale().toString(result.nodes),
                                   QString::number(result.solveSeconds, 'f', 2)));

    logLine(QStringLiteral("σv=%1 MPa · 位移=%2 μm · n=%3 · 单元 %4 / 节点 %5 · %6 s")
                .arg(result.peakVonMises, 0, 'f', 1)
                .arg(result.maxDeflection, 0, 'f', 1)
                .arg(result.safetyFactor, 0, 'f', 2)
                .arg(result.elements)
                .arg(result.nodes)
                .arg(result.solveSeconds, 0, 'f', 2));
    logLine(verdict);

    refreshActions();
    updateWindowTitle();
}

void MainWindow::openActivationDialog()
{
    ActivationDialog dialog(&m_license, this);
    connect(&dialog, &ActivationDialog::licenseChanged, this, [this] {
        refreshLicenseBadge();
        logLine(QStringLiteral("授权状态更新：%1").arg(m_license.current().message));
    });
    dialog.exec();
}

void MainWindow::showAbout()
{
    const QString demoNote = (m_backend && m_backend->isDemo())
                                 ? QStringLiteral("<p style='color:#B7352F'>当前为内置示意后端，"
                                                  "结果由经验公式估算，<b>不可用于工程判断</b>。</p>")
                                 : QString();
    QMessageBox::about(
        this, QStringLiteral("关于 QtCAE-QuickGear"),
        QStringLiteral("<h3>QtCAE-QuickGear %1</h3>"
                       "<p>轻量化齿轮 CAE 仿真工具 · Qt 桌面客户端</p>"
                       "<p>在线演示：<a href='%2'>%2</a><br/>"
                       "仓库：<a href='%3'>%3</a></p>"
                       "%4"
                       "<p style='color:#475569'>本机机器码：<code>%5</code></p>")
            .arg(QCoreApplication::applicationVersion(),
                 QString::fromLatin1(kWebsiteUrl),
                 QString::fromLatin1(kRepoUrl),
                 demoNote,
                 LicenseManager::machineCode()));
}

// ---------------------------------------------------------------- 工程文件

void MainWindow::newProject()
{
    if (!maybeSaveChanges())
        return;

    GearParams defaults;
    m_paramPanel->setParams(defaults);
    m_noteEdit->clear();
    m_mesh = StlMesh();
    m_stressView->clearMesh();
    m_stressView->setGear(defaults.teeth, defaults.pinionTeeth, defaults.module);
    m_stressView->clearResult();
    m_lastResult = SolveResult();
    m_hasResult  = false;
    m_projectPath.clear();
    m_dirty = false;

    resetMetrics();
    refreshActions();
    updateWindowTitle();
    m_stageLabel->setText(QStringLiteral("新建工程"));
    logLine(QStringLiteral("新建工程（默认参数）"));
}

void MainWindow::openProject()
{
    if (!maybeSaveChanges())
        return;

    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("打开工程"),
        m_projectPath.isEmpty() ? QString() : QFileInfo(m_projectPath).absolutePath(),
        projectFileFilter());
    if (path.isEmpty())
        return;
    loadFrom(path);
}

void MainWindow::openRecent()
{
    auto *action = qobject_cast<QAction *>(sender());
    if (!action)
        return;
    const QString path = action->data().toString();
    if (path.isEmpty() || !QFileInfo::exists(path)) {
        m_recentFiles.removeAll(path);
        storeSettings();
        QMessageBox::information(this, QStringLiteral("文件不存在"),
                                 QStringLiteral("工程已移动或删除：\n%1")
                                     .arg(QDir::toNativeSeparators(path)));
        return;
    }
    if (!maybeSaveChanges())
        return;
    loadFrom(path);
}

void MainWindow::saveProject()
{
    if (m_projectPath.isEmpty()) {
        saveProjectAs();
        return;
    }
    saveTo(m_projectPath);
}

void MainWindow::saveProjectAs()
{
    const QString suggested =
        m_projectPath.isEmpty()
            ? QDir(defaultExportDir())
                  .filePath(QStringLiteral("QuickGear-%1.qgpj")
                                .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmm"))))
            : m_projectPath;

    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("保存工程"),
                                                      suggested, projectFileFilter());
    if (path.isEmpty())
        return;
    saveTo(path);
}

bool MainWindow::saveTo(const QString &path)
{
    ProjectData data;
    data.params    = m_paramPanel->params();
    data.hasResult = m_hasResult;
    data.result    = m_lastResult;
    data.meshPath  = m_mesh.isEmpty() ? QString() : m_mesh.sourcePath;
    data.note      = m_noteEdit->text().trimmed();

    QString error;
    // 显式限定：本类也有一个同名的 saveProject 槽
    if (!::saveProject(path, data, &error)) {
        logLine(QStringLiteral("保存失败：%1").arg(error));
        QMessageBox::warning(this, QStringLiteral("保存失败"), error);
        return false;
    }

    m_projectPath = path;
    m_dirty       = false;
    rememberRecent(path);
    updateWindowTitle();
    refreshActions();
    m_stageLabel->setText(QStringLiteral("工程已保存"));
    logLine(QStringLiteral("已保存工程：%1").arg(QDir::toNativeSeparators(path)));
    return true;
}

bool MainWindow::loadFrom(const QString &path)
{
    ProjectData data;
    QString     error;
    if (!loadProject(path, &data, &error)) {
        logLine(QStringLiteral("打开失败：%1").arg(error));
        QMessageBox::warning(this, QStringLiteral("打开失败"), error);
        return false;
    }

    applyProject(data);

    m_projectPath = path;
    m_dirty       = false;
    rememberRecent(path);
    updateWindowTitle();
    refreshActions();

    logLine(QStringLiteral("已打开工程：%1").arg(QDir::toNativeSeparators(path)));
    m_stageLabel->setText(QStringLiteral("工程已载入%1")
                              .arg(m_hasResult ? QStringLiteral("，含上次结果") : QString()));
    return true;
}

void MainWindow::applyProject(const ProjectData &data)
{
    m_paramPanel->setParams(data.params);
    m_noteEdit->setText(data.note);
    m_stressView->setGear(data.params.teeth, data.params.pinionTeeth, data.params.module);
    m_stressView->clearResult();

    // 几何存路径：文件可能已被移动，读不到就降级为"无几何"并说清楚
    m_mesh = StlMesh();
    m_stressView->clearMesh();
    if (!data.meshPath.isEmpty()) {
        const StlLoadResult mesh = loadStl(data.meshPath);
        if (mesh.ok) {
            m_mesh = mesh.mesh;
            m_stressView->setMesh(m_mesh);
            logLine(QStringLiteral("已载入几何：%1").arg(stlSummary(m_mesh)));
        } else {
            logLine(QStringLiteral("几何未能载入：%1").arg(mesh.error));
        }
    }

    if (data.hasResult) {
        m_lastResult = data.result;
        m_hasResult  = true;
        m_stressView->setResult(data.result);
        m_sigmaLabel->setText(QString::number(data.result.peakVonMises, 'f', 1));
        m_dispLabel->setText(QString::number(data.result.maxDeflection, 'f', 1));
        m_meshLabel->setText(QLocale().toString(data.result.elements));
        m_safeLabel->setText(QString::number(data.result.safetyFactor, 'f', 2));
        bool passed = false;
        logLine(QStringLiteral("已恢复上次结果：%1").arg(conclusionText(data.result, &passed)));
    } else {
        m_lastResult = SolveResult();
        m_hasResult  = false;
        resetMetrics();
    }
}

bool MainWindow::maybeSaveChanges()
{
    if (!m_dirty)
        return true;

    const QMessageBox::StandardButton answer = QMessageBox::question(
        this, QStringLiteral("未保存的修改"),
        QStringLiteral("当前工程有未保存的修改，是否保存？"),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);

    if (answer == QMessageBox::Cancel)
        return false;
    if (answer == QMessageBox::Discard)
        return true;

    if (!m_projectPath.isEmpty())
        return saveTo(m_projectPath);

    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("保存工程"),
        QDir(defaultExportDir())
            .filePath(QStringLiteral("QuickGear-%1.qgpj")
                          .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmm")))),
        projectFileFilter());
    return path.isEmpty() ? false : saveTo(path);
}

void MainWindow::rememberRecent(const QString &path)
{
    m_recentFiles.removeAll(path);
    m_recentFiles.prepend(path);
    while (m_recentFiles.size() > 6)
        m_recentFiles.removeLast();
    storeSettings();
}

// ---------------------------------------------------------------- 几何与导出

void MainWindow::importStl()
{
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("导入 STL 几何"),
        m_mesh.isEmpty() ? defaultExportDir() : QFileInfo(m_mesh.sourcePath).absolutePath(),
        QStringLiteral("STL 网格 (*.stl);;所有文件 (*.*)"));
    if (path.isEmpty())
        return;
    importStlFrom(path);
}

bool MainWindow::importStlFrom(const QString &path)
{
    const StlLoadResult result = loadStl(path);
    if (!result.ok) {
        logLine(QStringLiteral("导入失败：%1").arg(result.error));
        QMessageBox::warning(this, QStringLiteral("导入失败"), result.error);
        return false;
    }

    m_mesh = result.mesh;
    m_stressView->setMesh(m_mesh);
    m_dirty = true;

    logLine(QStringLiteral("已导入几何（%1）：%2")
                .arg(result.format == StlLoadResult::Binary ? QStringLiteral("二进制")
                                                            : QStringLiteral("ASCII"),
                     stlSummary(m_mesh)));
    if (m_hasResult)
        logLine(QStringLiteral("注意：几何仅用于预览，当前结果仍基于参数化齿轮重新求解"));

    refreshActions();
    updateWindowTitle();
    m_stageLabel->setText(QStringLiteral("已导入几何 · 待求解"));
    return true;
}

QString MainWindow::defaultExportDir() const
{
    if (!m_projectPath.isEmpty())
        return QFileInfo(m_projectPath).absolutePath();

    const QString desktop = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
    return desktop.isEmpty() ? QDir::homePath() : desktop;
}

QImage MainWindow::renderStressImage() const
{
    // 导出用离屏大图：直接抓屏会把 460px 的视口缩到 PDF 里，图例与标注糊掉
    StressView shot;
    shot.setAttribute(Qt::WA_DontShowOnScreen, true);
    shot.resize(760, 600);

    const GearParams p = m_paramPanel->params();
    shot.setGear(p.teeth, p.pinionTeeth, p.module);
    shot.setDemoMode(m_backend && m_backend->isDemo());
    if (!m_mesh.isEmpty())
        shot.setMesh(m_mesh);
    shot.setResult(m_lastResult);
    shot.show();
    return shot.grab().toImage();
}

QString MainWindow::buildReportContextAndExport(bool pdf)
{
    if (!m_hasResult)
        return QString();

    const LicenseInfo info = m_license.current();

    ReportContext ctx;
    ctx.params      = m_paramPanel->params();
    ctx.result      = m_lastResult;
    ctx.licensed    = info.valid;
    ctx.licensee    = info.licensee;
    ctx.demo        = m_backend && m_backend->isDemo();
    ctx.machineCode = LicenseManager::machineCode();
    ctx.meshSummary = m_mesh.isEmpty() ? QString() : stlSummary(m_mesh);
    ctx.note        = m_noteEdit->text().trimmed();
    ctx.appVersion  = QCoreApplication::applicationVersion();

    QImage image = renderStressImage();
    if (!ctx.licensed)
        image = watermarkTrial(image, QStringLiteral("未授权试用 · 示意数据"));
    ctx.stressImage = image;

    const QString baseName =
        m_projectPath.isEmpty()
            ? QStringLiteral("QuickGear-结论-%1")
                  .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmm")))
            : QFileInfo(m_projectPath).completeBaseName() + QStringLiteral("-结论");

    const QString path = QFileDialog::getSaveFileName(
        this, pdf ? QStringLiteral("导出结论 PDF") : QStringLiteral("导出云图 PNG"),
        QDir(defaultExportDir()).filePath(baseName + (pdf ? QStringLiteral(".pdf")
                                                         : QStringLiteral(".png"))),
        pdf ? QStringLiteral("PDF 文档 (*.pdf)") : QStringLiteral("PNG 图片 (*.png)"));
    if (path.isEmpty())
        return QString();

    QString error;
    const bool ok = pdf ? writeReportPdf(path, ctx, &error) : writeImagePng(path, image, &error);
    if (!ok) {
        logLine(QStringLiteral("导出失败：%1").arg(error));
        QMessageBox::warning(this, QStringLiteral("导出失败"), error);
        return QString();
    }

    logLine(QStringLiteral("已导出 %1：%2")
                .arg(pdf ? QStringLiteral("PDF") : QStringLiteral("PNG"),
                     QDir::toNativeSeparators(path)));
    statusBar()->showMessage(QStringLiteral("已导出：%1").arg(QDir::toNativeSeparators(path)), 8000);
    return path;
}

void MainWindow::exportPdf()
{
    buildReportContextAndExport(true);
}

void MainWindow::exportPng()
{
    buildReportContextAndExport(false);
}

// ---------------------------------------------------------------- 拖放与关闭

void MainWindow::dragEnterEvent(QDragEnterEvent *event)
{
    if (!event->mimeData()->hasUrls())
        return;
    for (const QUrl &url : event->mimeData()->urls()) {
        const QString path = url.toLocalFile();
        if (hasSuffix(path, ".qgpj") || hasSuffix(path, ".stl")) {
            event->acceptProposedAction();
            return;
        }
    }
}

void MainWindow::dropEvent(QDropEvent *event)
{
    for (const QUrl &url : event->mimeData()->urls()) {
        const QString path = url.toLocalFile();
        if (hasSuffix(path, ".qgpj")) {
            if (maybeSaveChanges())
                loadFrom(path);
            event->acceptProposedAction();
            return;
        }
        if (hasSuffix(path, ".stl")) {
            importStlFrom(path);
            event->acceptProposedAction();
            return;
        }
    }
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (!maybeSaveChanges()) {
        event->ignore();
        return;
    }
    storeSettings();
    event->accept();
}

// ---------------------------------------------------------------- 配置持久化

void MainWindow::restoreSettings()
{
    QSettings settings;

    restoreGeometry(settings.value(QStringLiteral("window/geometry")).toByteArray());
    m_recentFiles = settings.value(QStringLiteral("files/recent")).toStringList();

    GearParams p;
    p.teeth       = settings.value(QStringLiteral("params/teeth"), p.teeth).toInt();
    p.pinionTeeth = settings.value(QStringLiteral("params/pinionTeeth"), p.pinionTeeth).toInt();
    p.module      = settings.value(QStringLiteral("params/module"), p.module).toDouble();
    p.torque      = settings.value(QStringLiteral("params/torque"), p.torque).toDouble();
    p.faceWidth   = settings.value(QStringLiteral("params/faceWidth"), p.faceWidth).toDouble();
    p.material    = settings.value(QStringLiteral("params/material"), p.material).toString();

    m_paramPanel->setParams(p);
    m_noteEdit->setText(settings.value(QStringLiteral("params/note")).toString());
    m_stressView->setGear(p.teeth, p.pinionTeeth, p.module);
    m_dirty = false;
}

void MainWindow::storeSettings()
{
    QSettings settings;
    const GearParams p = m_paramPanel->params();

    settings.setValue(QStringLiteral("window/geometry"), saveGeometry());
    settings.setValue(QStringLiteral("params/teeth"), p.teeth);
    settings.setValue(QStringLiteral("params/pinionTeeth"), p.pinionTeeth);
    settings.setValue(QStringLiteral("params/module"), p.module);
    settings.setValue(QStringLiteral("params/torque"), p.torque);
    settings.setValue(QStringLiteral("params/faceWidth"), p.faceWidth);
    settings.setValue(QStringLiteral("params/material"), p.material);
    settings.setValue(QStringLiteral("params/note"), m_noteEdit->text());
    settings.setValue(QStringLiteral("files/recent"), m_recentFiles);
}
