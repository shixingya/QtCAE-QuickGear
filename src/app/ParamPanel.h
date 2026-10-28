#pragma once

#include "core/GearParams.h"

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
    void setParams(const GearParams &params);
    void setEditable(bool editable);

signals:
    void paramsChanged();

private:
    /** 刷新分度圆 / 齿顶圆等派生尺寸提示 */
    void updateDimensionHint();

    QSpinBox       *m_teeth      = nullptr;
    QSpinBox       *m_pinion     = nullptr;
    QDoubleSpinBox *m_module     = nullptr;
    QDoubleSpinBox *m_torque     = nullptr;
    QDoubleSpinBox *m_faceWidth  = nullptr;
    QComboBox      *m_material   = nullptr;
    QLabel         *m_dimension  = nullptr;   // 分度圆 / 齿顶圆提示
};
