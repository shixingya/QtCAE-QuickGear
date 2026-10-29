#include "MockSolverBackend.h"

#include <QElapsedTimer>
#include <QThread>
#include <QtMath>

namespace {

/** 各阶段的假进度耗时（ms），总时长约 1.2s，接近真实客户端的"可感知但不烦人"区间 */
struct Stage
{
    int     percent;
    QString text;
    int     ms;
};

const QVector<Stage> kStages = {
    {  5, QStringLiteral("校验输入参数"),            80 },
    { 15, QStringLiteral("离散齿廓几何"),            140 },
    { 32, QStringLiteral("AI 辅助网格生成"),         260 },
    { 55, QStringLiteral("装配刚度矩阵"),            220 },
    { 74, QStringLiteral("静力求解（PCG 迭代）"),     320 },
    { 92, QStringLiteral("结果后处理 / 云图映射"),    140 },
};

} // namespace

double allowableStressMPa(const QString &material)
{
    // 材料表在 GearParamLimits 里：界面下拉、工程文件校验、这里取许用都看同一份。
    // 网页端仍通过这个函数拿数（自检会拿 docs/index.html 里的常量对账）。
    return GearParamLimits::allowableStressMPa(material);
}

QString MockSolverBackend::displayName() const
{
    return QStringLiteral("内置示意后端（未接入求解器）");
}

SolveResult MockSolverBackend::solve(const GearParams &params, const ProgressFn &onProgress)
{
    QElapsedTimer timer;
    timer.start();

    SolveResult result;
    result.backend = displayName();

    if (params.teeth < 8 || params.pinionTeeth < 6) {
        result.ok    = false;
        result.error = QStringLiteral("齿数过小，无法构建齿轮副：z=%1 / z2=%2")
                           .arg(params.teeth).arg(params.pinionTeeth);
        return result;
    }
    if (params.module <= 0.0 || params.torque <= 0.0) {
        result.ok    = false;
        result.error = QStringLiteral("模数与扭矩必须为正数");
        return result;
    }

    for (const Stage &stage : kStages) {
        if (onProgress)
            onProgress(stage.percent, stage.text);
        QThread::msleep(static_cast<unsigned long>(stage.ms));
    }

    const double z = params.teeth;
    const double m = params.module;
    const double T = params.torque;

    // 峰值应力：与网页端同一套经验式，保证"网页预览 ≈ 客户端展示"体感一致。
    // 截断线 640 = 量程 320 的两倍：网页端 docs/index.html 的 estimateResults
    // 用的是同一个规则，改这里必须同步改那边，否则同一个参数两边给出两个数。
    const double sigma = 12.0 * T / qPow(m, 3.0) * qSqrt(24.0 / z);
    const double limit = GearParamLimits::allowableStressMPa(params.material);

    result.peakVonMises  = qMin(sigma, 640.0);
    result.maxDeflection = result.peakVonMises * 0.09;
    result.elements      = static_cast<int>(18000.0 * qPow(z / 24.0, 1.15) * qPow(2.0 / m, 1.6));
    result.nodes         = static_cast<int>(result.elements * 1.42);
    result.safetyFactor  = limit / result.peakVonMises;
    result.stressCeiling = 320.0;

    // 逐齿峰值：接触齿最高，相邻两齿次之，其余按载荷分配衰减
    result.toothStress.resize(params.teeth);
    for (int i = 0; i < params.teeth; ++i) {
        // 让最大值落在一个确定齿位上（接触区），形成可辨认的云图热斑
        int distance = qMin(i, params.teeth - i);           // 距离接触齿的齿数
        double falloff = qExp(-distance * 0.55);
        result.toothStress[i] = result.peakVonMises * (0.18 + 0.82 * falloff);
    }

    if (onProgress)
        onProgress(100, QStringLiteral("完成"));

    result.solveSeconds = timer.elapsed() / 1000.0;
    result.ok = true;
    return result;
}
