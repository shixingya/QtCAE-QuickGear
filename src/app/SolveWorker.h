#pragma once

#include "core/ISolverBackend.h"

#include <QObject>

#include <memory>

/**
 * 求解工作对象：被 moveToThread 到工作线程，避免阻塞 UI。
 * 进度与结果都通过信号回到主线程（跨线程自动排队）。
 */
class SolveWorker : public QObject
{
    Q_OBJECT

public:
    // 持一份 shared_ptr：即使窗口在求解途中被析构，后端也活到 solve() 返回为止
    SolveWorker(std::shared_ptr<ISolverBackend> backend, const GearParams &params,
                QObject *parent = nullptr);

public slots:
    void run();

signals:
    void progress(int percent, const QString &stage);
    void finished(const SolveResult &result);

private:
    std::shared_ptr<ISolverBackend> m_backend;
    GearParams                      m_params;
};
