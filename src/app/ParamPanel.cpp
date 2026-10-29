#include "ParamPanel.h"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QVBoxLayout>

ParamPanel::ParamPanel(QWidget *parent)
    : QWidget(parent)
{
    auto *box = new QGroupBox(QStringLiteral("齿轮副参数"), this);

    // 区间数字只从 GearParamLimits 取：工程文件读取用的也是同一份，
    // 两边各写一套迟早出现"界面能填 80，打开文件被改成 80 以外"的怪事
    m_teeth = new QSpinBox(box);
    m_teeth->setObjectName(QStringLiteral("teethSpin"));
    m_teeth->setRange(GearParamLimits::kTeethMin, GearParamLimits::kTeethMax);
    m_teeth->setSingleStep(2);
    m_teeth->setSuffix(QStringLiteral(" 齿"));
    m_teeth->setValue(24);

    m_pinion = new QSpinBox(box);
    m_pinion->setObjectName(QStringLiteral("pinionSpin"));
    m_pinion->setRange(GearParamLimits::kPinionMin, GearParamLimits::kPinionMax);
    m_pinion->setSingleStep(2);
    m_pinion->setSuffix(QStringLiteral(" 齿"));
    m_pinion->setValue(12);

    m_module = new QDoubleSpinBox(box);
    m_module->setObjectName(QStringLiteral("moduleSpin"));
    m_module->setRange(GearParamLimits::kModuleMin, GearParamLimits::kModuleMax);
    m_module->setSingleStep(0.5);
    // 小数位就是精度：spinbox 装不下的值会被当场四舍五入，而模数在 σ ∝ 1/m³ 里
    // 当分母——1 位小数把 2.25 变成 2.3，峰值应力就少算 6%。计量到 0.01 mm 就够。
    // 位数写在 GearDisplay 里：日志、报告、这里必须是同一份，否则报告上的数复盘不出来。
    m_module->setDecimals(GearDisplay::kModuleDecimals);
    m_module->setSuffix(QStringLiteral(" mm"));
    m_module->setValue(2.0);

    m_torque = new QDoubleSpinBox(box);
    m_torque->setObjectName(QStringLiteral("torqueSpin"));
    m_torque->setRange(GearParamLimits::kTorqueMin, GearParamLimits::kTorqueMax);
    m_torque->setSingleStep(5.0);
    // 扭矩直接乘在 σ 上：整数位会把 87.5 N·m 变成 88（–0.6%），留一位小数
    m_torque->setDecimals(GearDisplay::kTorqueDecimals);
    m_torque->setSuffix(QStringLiteral(" N·m"));
    m_torque->setValue(80.0);

    m_faceWidth = new QDoubleSpinBox(box);
    m_faceWidth->setObjectName(QStringLiteral("faceWidthSpin"));
    m_faceWidth->setRange(GearParamLimits::kFaceWidthMin, GearParamLimits::kFaceWidthMax);
    m_faceWidth->setSingleStep(1.0);
    m_faceWidth->setDecimals(GearDisplay::kFaceWidthDecimals);
    m_faceWidth->setSuffix(QStringLiteral(" mm"));
    m_faceWidth->setValue(20.0);

    m_material = new QComboBox(box);
    // 材料表只在 GearParamLimits 里写一份：界面下拉、工程文件校验、后端取许用应力都用它
    m_material->addItems(GearParamLimits::materials());

    auto *form = new QFormLayout;
    form->setLabelAlignment(Qt::AlignLeft);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->addRow(QStringLiteral("主动轮齿数 z₁"), m_teeth);
    form->addRow(QStringLiteral("从动轮齿数 z₂"), m_pinion);
    form->addRow(QStringLiteral("模数 m"), m_module);
    form->addRow(QStringLiteral("输入扭矩 T"), m_torque);
    form->addRow(QStringLiteral("齿宽 b"), m_faceWidth);
    form->addRow(QStringLiteral("材料"), m_material);

    m_dimension = new QLabel(box);
    m_dimension->setObjectName(QStringLiteral("dimensionHint"));
    m_dimension->setWordWrap(true);

    m_warning = new QLabel(box);
    m_warning->setObjectName(QStringLiteral("paramWarning"));
    m_warning->setWordWrap(true);
    m_warning->setVisible(false);

    auto *boxLayout = new QVBoxLayout(box);
    boxLayout->addLayout(form);
    boxLayout->addWidget(m_dimension);
    boxLayout->addWidget(m_warning);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->addWidget(box);
    root->addStretch(1);

    connect(m_teeth, QOverload<int>::of(&QSpinBox::valueChanged), this, [this] {
        // 这里曾把从动轮的上限压到 z1/2：改一次主动轮齿数，从动轮就会被静默夹小
        // （工程里 30 齿的从动轮变成 15 齿，传动比、报告、云图全部跟着变）。
        // 区间只认 kPinionMin..kPinionMax；z2 比 z1 大（增速传动）也是合法的，
        // 真要提醒就写在提示里，不动用户填的数。
        updateDimensionHint();
        emit paramsChanged();
    });
    connect(m_pinion, QOverload<int>::of(&QSpinBox::valueChanged), this, [this] {
        updateDimensionHint();
        emit paramsChanged();
    });
    connect(m_module, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this] {
        updateDimensionHint();
        emit paramsChanged();
    });
    connect(m_torque, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, &ParamPanel::paramsChanged);
    connect(m_faceWidth, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this] {
        updateDimensionHint();
        emit paramsChanged();
    });
    connect(m_material, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &ParamPanel::paramsChanged);

    updateDimensionHint();
}

GearParams ParamPanel::params() const
{
    GearParams p;
    p.teeth       = m_teeth->value();
    p.pinionTeeth = m_pinion->value();
    p.module      = m_module->value();
    p.torque      = m_torque->value();
    p.faceWidth   = m_faceWidth->value();
    p.material    = m_material->currentText();
    return p;
}

QStringList ParamPanel::setParams(const GearParams &params)
{
    const QSignalBlocker b1(m_teeth);
    const QSignalBlocker b2(m_pinion);
    const QSignalBlocker b3(m_module);
    const QSignalBlocker b4(m_torque);
    const QSignalBlocker b5(m_faceWidth);
    const QSignalBlocker b6(m_material);

    m_teeth->setValue(params.teeth);
    m_pinion->setValue(params.pinionTeeth);
    m_module->setValue(params.module);
    m_torque->setValue(params.torque);
    m_faceWidth->setValue(params.faceWidth);

    const int index = m_material->findText(params.material);
    // 认不出来的名字不装没看见：退到默认材料，并在参数提示里说一声（读文件时 core 已归一化）
    m_material->setCurrentIndex(index >= 0 ? index : 0);

    updateDimensionHint();

    // 装进去之后回头比一遍：spinbox 按 decimals 四舍五入，装不下的值当场就变了。
    // 齿轮 CAE 里模数和扭矩都在公式里，改 2% 就是结论从“边界”变“通过”，
    // 所以这里不静静吞掉——把改过的字段交回给调用方去写日志。
    QStringList notes;
    const GearParams shown = this->params();
    auto report = [&notes](const QString &label, double wanted, double got, const QString &unit) {
        if (qAbs(wanted - got) <= 1e-9)
            return;
        notes << QStringLiteral("%1 %2%3 → %4%5（界面精度装不下，已按显示值计算）")
                     .arg(label, QString::number(wanted, 'g', 10), unit,
                          QString::number(got, 'g', 10), unit);
    };
    report(QStringLiteral("模数"), params.module, shown.module, QStringLiteral(" mm"));
    report(QStringLiteral("输入扭矩"), params.torque, shown.torque, QStringLiteral(" N·m"));
    report(QStringLiteral("齿宽"), params.faceWidth, shown.faceWidth, QStringLiteral(" mm"));
    return notes;
}

void ParamPanel::setEditable(bool editable)
{
    m_teeth->setEnabled(editable);
    m_pinion->setEnabled(editable);
    m_module->setEnabled(editable);
    m_torque->setEnabled(editable);
    m_faceWidth->setEnabled(editable);
    m_material->setEnabled(editable);
}

void ParamPanel::updateDimensionHint()
{
    const GearParams p = params();
    m_dimension->setText(QStringLiteral("分度圆 d₁ = %1 mm\n齿顶圆 dₐ₁ = %2 mm\n传动比 i = %3")
                             .arg(p.pitchDiameter(), 0, 'f', 2)
                             .arg(p.tipDiameter(), 0, 'f', 2)
                             .arg(p.pinionTeeth > 0 ? double(p.teeth) / p.pinionTeeth : 0.0, 0, 'f', 2));

    // 只做"该看一眼"的提醒，不拦截：真实工程判断需要接触比、齿形修形等更多输入
    QStringList warnings;
    const double ratio = p.module > 0.0 ? p.faceWidth / p.module : 0.0;
    if (ratio < 8.0)
        warnings << QStringLiteral("齿宽偏窄（b/m = %1），齿向载荷集中会更明显").arg(ratio, 0, 'f', 1);
    else if (ratio > 25.0)
        warnings << QStringLiteral("齿宽偏宽（b/m = %1），需齿向修形或降低载荷").arg(ratio, 0, 'f', 1);
    if (p.pinionTeeth < 17)
        warnings << QStringLiteral("从动轮 %1 齿 < 17，标准直齿存在根切风险").arg(p.pinionTeeth);
    if (p.pinionTeeth > 0 && double(p.teeth) / p.pinionTeeth > 6.3)
        warnings << QStringLiteral("单级传动比 i = %1 偏大，建议拆为两级")
                        .arg(double(p.teeth) / p.pinionTeeth, 0, 'f', 2);

    m_warning->setVisible(!warnings.isEmpty());
    m_warning->setText(warnings.join(QStringLiteral("\n")));
}
