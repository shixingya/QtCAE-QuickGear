#pragma once

#include <QRect>
#include <QSize>
#include <QVector>

/**
 * 窗口位置复原时的两个小判断。
 *
 * 现场：笔记本接了外接屏，把窗口拖到副屏上关闭；第二天拔掉外接屏再开机，
 * 窗口按上次的位置恢复到一块已经不存在的屏幕上——任务栏里有进程，屏幕上什么都没有，
 * 用户只能去任务管理器杀掉。这里把"够不够得着"和"拉回哪里"写成纯函数，
 * 不用开窗口也能断言。
 */
namespace screenfit {

// 至少要有这么大一块落在某块屏幕里，用户才可能把它拖回来。
// 要求太严会把"故意只露一条边"的窗口也拉回中间，太松则夹缝里那条边根本点不到。
constexpr int kMinVisibleW = 160;
constexpr int kMinVisibleH = 90;

/** frame 是否至少有一块可见面积落在 available 里的某块屏幕上 */
inline bool reachable(const QRect &frame, const QVector<QRect> &available)
{
    for (const QRect &screen : available) {
        const QRect overlap = screen.intersected(frame);
        if (overlap.width() >= kMinVisibleW && overlap.height() >= kMinVisibleH)
            return true;
    }
    return false;
}

/**
 * 该用哪个矩形。够得着就原样返回；够不着就按 fallbackSize 居中到第一块屏幕
 * （调用方按主屏在前的顺序传 available）。一块屏幕都没有时原样返回。
 */
inline QRect fit(const QRect &frame, const QVector<QRect> &available, const QSize &fallbackSize)
{
    if (available.isEmpty() || reachable(frame, available))
        return frame;

    const QRect home = available.first();
    const QSize size = frame.size().isEmpty() ? fallbackSize : frame.size();
    QRect out(QPoint(0, 0), size);
    out.moveCenter(home.center());
    return out;
}

} // namespace screenfit
