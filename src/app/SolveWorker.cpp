#include "SolveWorker.h"

#include <utility>

SolveWorker::SolveWorker(std::shared_ptr<ISolverBackend> backend, const GearParams &params,
                         QObject *parent)
    : QObject(parent)
    , m_backend(std::move(backend))
    , m_params(params)
{
}

void SolveWorker::run()
{
    if (!m_backend) {
        SolveResult failed;
        failed.ok    = false;
        failed.error = QStringLiteral("求解后端未初始化");
        emit finished(failed);
        return;
    }

    const SolveResult result = m_backend->solve(m_params, [this](int percent, const QString &stage) {
        emit progress(percent, stage);
    });

    emit finished(result);
}
