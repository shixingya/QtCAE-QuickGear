#pragma once

#include "GearParams.h"

#include <QMetaType>
#include <QString>
#include <QVector>

#include <functional>

/**
 * 求解结果。
 * 注意：字段语义与闭源求解器保持一致，壳工程只做展示。
 */
struct SolveResult
{
    bool    ok            = false;
    QString error;                       // ok == false 时的原因
    QString backend;                     // 产生该结果的后端名称

    double  peakVonMises  = 0.0;         // 峰值等效应力 σv，MPa
    double  maxDeflection = 0.0;         // 最大位移，μm
    int     elements      = 0;           // 单元数
    int     nodes         = 0;           // 节点数
    double  safetyFactor  = 0.0;         // 安全系数 n
    double  solveSeconds  = 0.0;         // 求解耗时，s

    QVector<double> toothStress;         // 各齿峰值应力，用于云图着色
    double          stressCeiling = 320.0; // 云图量程上限，MPa
};

Q_DECLARE_METATYPE(SolveResult)

/**
 * 求解后端抽象。
 *
 * 闭源实现（加密静态库）与内置 Mock 都实现该接口，
 * 界面层不感知具体是哪一个，从而保证壳工程可独立编译。
 */
class ISolverBackend
{
public:
    /** 进度回调：百分比 0~100 + 阶段描述。可能在非 UI 线程被调用。 */
    using ProgressFn = std::function<void(int percent, const QString &stage)>;

    virtual ~ISolverBackend() = default;

    /** 后端名称，显示在标题栏与状态栏 */
    virtual QString displayName() const = 0;

    /** 是否为示意实现（结果不可用于工程判断） */
    virtual bool isDemo() const = 0;

    /** 执行一次静力求解。阻塞调用，由 SolveWorker 放到工作线程。 */
    virtual SolveResult solve(const GearParams &params, const ProgressFn &onProgress) = 0;
};
