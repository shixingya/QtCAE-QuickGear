#pragma once

#include <QMetaType>
#include <QString>
#include <QStringList>

/**
 * 参数合法区间。
 *
 * 界面上的 spinbox 和工程文件读取共用这一份数字：以前区间只写在 ParamPanel 里，
 * 而 loadProject 什么都不查，别人发来的 .qgpj 写 z=0 或 m=-1 照样能打开，
 * 然后在求解/绘图那里变成一个莫名其妙的数。
 */
struct GearParamLimits
{
    static constexpr int    kTeethMin     = 10;
    static constexpr int    kTeethMax     = 80;
    static constexpr int    kPinionMin    = 6;
    static constexpr int    kPinionMax    = 40;
    static constexpr double kModuleMin    = 0.5;
    static constexpr double kModuleMax    = 8.0;
    static constexpr double kTorqueMin    = 5.0;
    static constexpr double kTorqueMax    = 800.0;
    static constexpr double kFaceWidthMin = 4.0;
    static constexpr double kFaceWidthMax = 120.0;
};

/**
 * 齿轮计算输入参数。
 * 单位约定：长度 mm，扭矩 N·m，应力 MPa。
 */
struct GearParams
{
    int    teeth      = 24;                        // 主动轮齿数
    int    pinionTeeth = 12;                       // 从动轮齿数（偶数，便于对齿）
    double module     = 2.0;                       // 模数 m
    double torque     = 80.0;                      // 输入扭矩 T
    double faceWidth  = 20.0;                      // 齿宽 b
    QString material  = QStringLiteral("20CrMnTi");// 材料牌号（影响许用应力）

    /** 分度圆直径 d = m·z */
    double pitchDiameter() const { return module * teeth; }
    /** 齿顶圆直径 da = m·(z+2) */
    double tipDiameter() const { return module * (teeth + 2); }
};

/**
 * 把外部来源（工程文件 / 命令行 / 手改过的 JSON）的参数夹进合法区间。
 *
 * 不做静默修正：返回被改动的字段与原始值，调用方负责告诉用户“我改了你的数”。
 * 从动轮上限还跟着主动轮走（z₂ 不应大于 z₁/2，和界面里的联动一致）。
 */
QStringList clampToLimits(GearParams *params);

Q_DECLARE_METATYPE(GearParams)
