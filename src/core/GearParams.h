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

    /** 未知材料的许用应力。只在材料已被归一化之后的兜底路径里用到 */
    static constexpr double kUnknownAllowable = 350.0;

    /**
     * 材料表：界面下拉、工程文件校验、求解后端取许用应力共用这一份。
     *
     * 以前三个地方各写一套：下拉列出四个牌号，后端靠 contains("40Cr") 猜许用，
     * 工程文件里写个表以外的名字就静静拿到 350 MPa，界面也不改——报告上的材料
     * 和算出来的安全系数可以属于两种不同的钢。
     */
    static QStringList materials();
    static QString     defaultMaterial();

    /** 把写法归一化到材料表里的标准名；认不出来返回空 */
    static QString canonicalMaterial(const QString &raw);

    /** 许用应力（MPa）。认不出来的名字返回 kUnknownAllowable */
    static double allowableStressMPa(const QString &raw);
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
    QString material  = QStringLiteral("20CrMnTi 渗碳淬火");  // 材料牌号（影响许用应力）

    /** 分度圆直径 d = m·z */
    double pitchDiameter() const { return module * teeth; }
    /** 齿顶圆直径 da = m·(z+2) */
    double tipDiameter() const { return module * (teeth + 2); }
};

/**
 * 把外部来源（工程文件 / 命令行 / 手改过的 JSON）的参数夹进合法区间。
 *
 * 不做静默修正：返回被改动的字段与原始值，调用方负责告诉用户“我改了你的数”。
 * 只认区间，不额外约束从动轮与主动轮的比值：z₂ > z₁ 是增速传动，属于合法工况。
 */
QStringList clampToLimits(GearParams *params);

Q_DECLARE_METATYPE(GearParams)
