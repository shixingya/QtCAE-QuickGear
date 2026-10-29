#pragma once

#include <QString>

/**
 * 云图色标的饱和判定。
 *
 * 色标上限与网页端保持一致，固定 320 MPa；而峰值可以到 640（截断线）。
 * 一旦峰值超过上限，图上最热的那几个齿会被压成同一个颜色——客户读图时会把
 * “最红”当成色标顶端的 320，而实际可能是 480，同一张图上两个差 150 MPa 的
 * 齿看起来一模一样。
 *
 * 这里把判定与措辞收成一处：视图、报告、自检都用它，免得三处各写一套阈值。
 */
namespace stressscale {

/**
 * 峰值是否已顶到色标上限。
 * ceiling <= 0 视作量程未知，不报饱和（宁可不提示，也不要报一个假的）。
 * 0.5 的余量是因为刻度文字按整数显示：320.4 印出来还是 320。
 */
inline bool saturated(double peak, double ceiling)
{
    return ceiling > 0.0 && peak > ceiling + 0.5;
}

/**
 * 给用户看的一句话；不饱和时返回空串（调用方据此决定要不要占地方）。
 * 措辞要说清两件事：真实峰值是多少、红色区实际代表的不是一个点而是“≥上限”。
 */
inline QString saturationNote(double peak, double ceiling)
{
    if (!saturated(peak, ceiling))
        return QString();
    return QStringLiteral("峰值 %1 MPa 超出色标量程 · 红色区实际 ≥ %2")
        .arg(QString::number(peak, 'f', 0), QString::number(ceiling, 'f', 0));
}

} // namespace stressscale
