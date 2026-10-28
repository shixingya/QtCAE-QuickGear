#include "MainWindow.h"

#include "ActivationDialog.h"
#include "ParamPanel.h"
#include "SolveWorker.h"
#include "StressView.h"
#include "core/SolverFactory.h"

#include <QAction>
#include <QApplication>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QLocale>
#include <QMenuBar>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QStatusBar>
#include <QThread>
#include <QUrl>
#include <QVBoxLayout>

namespace {

const char *kRepoUrl = "https://github.com/shixingya/QtCAE-QuickGear";

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

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    m_backend.reset(createSolverBackend());

    buildUi();
    buildMenus();

    refreshBackendBadge();
    refreshLicenseBadge();
    resetMetrics();

    m_stressView->setGear(m_paramPanel->params().teeth,
                          m_paramPanel->params().pinionTeeth,
                          m_paramPanel->params().module);
    m_stressView->setDemoMode(m_backend && m_backend->isDemo());
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

    // ---- 左：参数面板 + 运行按钮 ----
    m_paramPanel = new ParamPanel(central);
    m_paramPanel->setFixedWidth(288);

    m_runButton = new QPushButton(QStringLiteral("运行仿真"), central);
    m_runButton->setObjectName(QStringLiteral("runButton"));
    m_runButton->setMinimumHeight(38);
    m_runButton->setCursor(Qt::PointingHandCursor);
    connect(m_runButton, &QPushButton::clicked, this, &MainWindow::runSolve);
    connect(m_paramPanel, &ParamPanel::paramsChanged, this, &MainWindow::onParamsChanged);

    auto *leftColumn = new QVBoxLayout;
    leftColumn->setContentsMargins(0, 0, 0, 0);
    leftColumn->setSpacing(12);
    leftColumn->addWidget(m_paramPanel);
    leftColumn->addWidget(m_runButton);
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
    resize(1120, 720);

    // ---- 状态栏 ----
    m_stageLabel   = new QLabel(this);
    m_licenseLabel = new QLabel(this);
    m_licenseLabel->setObjectName(QStringLiteral("licenseBadge"));
    statusBar()->addWidget(m_stageLabel, 1);
    statusBar()->addPermanentWidget(m_licenseLabel);
}

void MainWindow::buildMenus()
{
    auto *fileMenu = menuBar()->addMenu(QStringLiteral("文件(&F)"));
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

    auto *helpMenu = menuBar()->addMenu(QStringLiteral("帮助(&H)"));
    auto *repoAction = helpMenu->addAction(QStringLiteral("打开 GitHub 仓库"));
    connect(repoAction, &QAction::triggered, this, [] {
        QDesktopServices::openUrl(QUrl(QString::fromLatin1(kRepoUrl)));
    });
    auto *aboutAction = helpMenu->addAction(QStringLiteral("关于"));
    connect(aboutAction, &QAction::triggered, this, &MainWindow::showAbout);
}

void MainWindow::refreshBackendBadge()
{
    const QString backendName = m_backend ? m_backend->displayName() : QStringLiteral("未配置后端");
    const bool demo = m_backend && m_backend->isDemo();

    setWindowTitle(QStringLiteral("QtCAE-QuickGear  %1 — %2")
                       .arg(QCoreApplication::applicationVersion(), backendName));
    m_stageLabel->setText(demo ? QStringLiteral("就绪 · %1（结果为示意数据）").arg(backendName)
                               : QStringLiteral("就绪 · %1").arg(backendName));
}

void MainWindow::refreshLicenseBadge()
{
    const LicenseInfo info = m_license.current();
    if (info.valid) {
        m_licenseLabel->setText(QStringLiteral("授权：已激活（%1）").arg(info.edition));
        m_licenseLabel->setStyleSheet(QStringLiteral("color:#059669;"));
    } else {
        m_licenseLabel->setText(QStringLiteral("授权：未激活（试用）"));
        m_licenseLabel->setStyleSheet(QStringLiteral("color:#B4692F;"));
    }
    m_licenseLabel->setToolTip(info.message);
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
}

void MainWindow::onParamsChanged()
{
    const GearParams params = m_paramPanel->params();
    m_stressView->setGear(params.teeth, params.pinionTeeth, params.module);
    m_stressView->clearResult();
    resetMetrics();
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

    auto *thread = new QThread;
    auto *worker = new SolveWorker(m_backend.get(), m_paramPanel->params());
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
}

void MainWindow::onSolveFinished(const SolveResult &result)
{
    setBusy(false);

    if (!result.ok) {
        m_stageLabel->setText(QStringLiteral("求解失败"));
        QMessageBox::warning(this, QStringLiteral("求解失败"), result.error);
        return;
    }

    m_stressView->setResult(result);

    m_sigmaLabel->setText(QString::number(result.peakVonMises, 'f', 1));
    m_dispLabel->setText(QString::number(result.maxDeflection, 'f', 1));
    m_meshLabel->setText(QLocale().toString(result.elements));
    m_safeLabel->setText(QString::number(result.safetyFactor, 'f', 2));

    m_stageLabel->setText(QStringLiteral("求解完成 · %1 单元 / %2 节点 · 耗时 %3 s%4")
                              .arg(QLocale().toString(result.elements),
                                   QLocale().toString(result.nodes),
                                   QString::number(result.solveSeconds, 'f', 2),
                                   m_backend && m_backend->isDemo() ? QStringLiteral("（示意数据）")
                                                                    : QString()));
}

void MainWindow::openActivationDialog()
{
    ActivationDialog dialog(&m_license, this);
    connect(&dialog, &ActivationDialog::licenseChanged, this, &MainWindow::refreshLicenseBadge);
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
                       "<p>轻量化齿轮 CAE 仿真工具 · Qt 壳工程</p>"
                       "<p>仓库：<a href='%2'>%2</a></p>"
                       "%3"
                       "<p style='color:#475569'>本机机器码：<code>%4</code></p>")
            .arg(QCoreApplication::applicationVersion(),
                 QString::fromLatin1(kRepoUrl),
                 demoNote,
                 LicenseManager::machineCode()));
}
