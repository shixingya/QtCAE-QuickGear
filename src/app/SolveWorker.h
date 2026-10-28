#pragma once

#include "core/ISolverBackend.h"

#include <QObject>

/**
 * 求解工作对象：被 moveToThread 到工作线程，避免阻塞 UI。
 * 进度与结果都通过信号回到主线程（跨线程自动排队）。
 */
class SolveWorker : public QObject
{
    Q_OBJECT

public:
    SolveWorker(ISolverBackend *backend, const GearParams &params, QObject *parent = nullptr);

public slots:
    void run();

signals:
    void progress(int percent, const QString &stage);
    void finished(const SolveResult &result);

private:
    ISolverBackend *m_backend = nullptr;
    GearParams       m_params;
};
