#pragma once

#include "core/GearParams.h"

#include <QStringList>
#include <QWidget>

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QSpinBox;

/**
 * 左侧参数面板：齿轮副基本参数 + 材料。
 * 当前只有静态参数；后续接闭源库时在这里扩展齿形/修形选项。
 */
class ParamPanel : public QWidget
{
    Q_OBJECT

public:
    explicit ParamPanel(QWidget *parent = nullptr);

    GearParams params() const;

    /**
     * 把参数装进控件。
     *
     * 返回“界面装不下”的字段说明（空 = 一个数都没动）：spinbox 只能显示到
     * decimals 位，装进去就会四舍五入，而 params() 拿回来的是四舍五入后的值。
     * 调用方（打开工程时）要把这些说明写进日志，不能静静改掉别人的模数。
     */
    QStringList setParams(const GearParams &params);
    void setEditable(bool editable);

signals:
    void paramsChanged();

private:
    /** 刷新分度圆 / 齿顶圆等派生尺寸提示与工程性提醒 */
    void updateDimensionHint();

    QSpinBox       *m_teeth      = nullptr;
    QSpinBox       *m_pinion     = nullptr;
    QDoubleSpinBox *m_module     = nullptr;
    QDoubleSpinBox *m_torque     = nullptr;
    QDoubleSpinBox *m_faceWidth  = nullptr;
    QComboBox      *m_material   = nullptr;
    QLabel         *m_dimension  = nullptr;   // 分度圆 / 齿顶圆提示
    QLabel         *m_warning    = nullptr;   // 参数区间提醒（不拦截求解）
};
