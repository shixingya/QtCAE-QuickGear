#pragma once

#include <QMetaType>
#include <QString>

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

Q_DECLARE_METATYPE(GearParams)
