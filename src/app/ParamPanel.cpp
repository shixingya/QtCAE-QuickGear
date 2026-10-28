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

    m_teeth = new QSpinBox(box);
    m_teeth->setRange(10, 80);
    m_teeth->setSingleStep(2);
    m_teeth->setSuffix(QStringLiteral(" 齿"));
    m_teeth->setValue(24);

    m_pinion = new QSpinBox(box);
    m_pinion->setRange(6, 40);
    m_pinion->setSingleStep(2);
    m_pinion->setSuffix(QStringLiteral(" 齿"));
    m_pinion->setValue(12);

    m_module = new QDoubleSpinBox(box);
    m_module->setRange(0.5, 8.0);
    m_module->setSingleStep(0.5);
    m_module->setDecimals(1);
    m_module->setSuffix(QStringLiteral(" mm"));
    m_module->setValue(2.0);

    m_torque = new QDoubleSpinBox(box);
    m_torque->setRange(5.0, 800.0);
    m_torque->setSingleStep(5.0);
    m_torque->setDecimals(0);
    m_torque->setSuffix(QStringLiteral(" N·m"));
    m_torque->setValue(80.0);

    m_faceWidth = new QDoubleSpinBox(box);
    m_faceWidth->setRange(4.0, 120.0);
    m_faceWidth->setSingleStep(1.0);
    m_faceWidth->setDecimals(1);
    m_faceWidth->setSuffix(QStringLiteral(" mm"));
    m_faceWidth->setValue(20.0);

    m_material = new QComboBox(box);
    m_material->addItems({QStringLiteral("20CrMnTi 渗碳淬火"),
                          QStringLiteral("40Cr 调质"),
                          QStringLiteral("45 钢调质"),
                          QStringLiteral("尼龙 PA66")});

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

    auto *boxLayout = new QVBoxLayout(box);
    boxLayout->addLayout(form);
    boxLayout->addWidget(m_dimension);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->addWidget(box);
    root->addStretch(1);

    connect(m_teeth, QOverload<int>::of(&QSpinBox::valueChanged), this, [this] {
        m_pinion->setMaximum(qMax(6, m_teeth->value() / 2));
        updateDimensionHint();
        emit paramsChanged();
    });
    connect(m_pinion, QOverload<int>::of(&QSpinBox::valueChanged), this, &ParamPanel::paramsChanged);
    connect(m_module, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this] {
        updateDimensionHint();
        emit paramsChanged();
    });
    connect(m_torque, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, &ParamPanel::paramsChanged);
    connect(m_faceWidth, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, &ParamPanel::paramsChanged);
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

void ParamPanel::setParams(const GearParams &params)
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
    if (index >= 0)
        m_material->setCurrentIndex(index);

    updateDimensionHint();
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
}
