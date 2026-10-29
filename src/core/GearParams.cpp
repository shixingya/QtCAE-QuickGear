#include "GearParams.h"

#include <QtGlobal>

namespace {

// 夹取并记录：改了就留一条"原值 → 新值"，让调用方能如实写进日志
void clampInt(int *value, int low, int high, const QString &label, QStringList *notes)
{
    const int before = *value;
    *value = qBound(low, before, high);
    if (*value != before) {
        notes->append(QStringLiteral("%1 %2 → %3（允许 %4~%5）")
                          .arg(label)
                          .arg(before)
                          .arg(*value)
                          .arg(low)
                          .arg(high));
    }
}

void clampDouble(double *value, double low, double high, const QString &label,
                 QStringList *notes)
{
    const double before = *value;
    if (qIsNaN(before)) {
        *value = low;
        notes->append(QStringLiteral("%1 不是数字 → %2").arg(label).arg(low));
        return;
    }
    *value = qBound(low, before, high);
    if (!qFuzzyCompare(*value, before)) {
        notes->append(QStringLiteral("%1 %2 → %3（允许 %4~%5）")
                          .arg(label)
                          .arg(before)
                          .arg(*value)
                          .arg(low)
                          .arg(high));
    }
}

} // namespace

QStringList clampToLimits(GearParams *params)
{
    QStringList notes;
    if (!params)
        return notes;

    clampInt(&params->teeth, GearParamLimits::kTeethMin, GearParamLimits::kTeethMax,
             QStringLiteral("主动轮齿数"), &notes);
    clampInt(&params->pinionTeeth, GearParamLimits::kPinionMin, GearParamLimits::kPinionMax,
             QStringLiteral("从动轮齿数"), &notes);
    // 齿轮副联动：从动轮不该比主动轮的一半还多，和界面里 z₂ 的上限同理
    const int pinionCap = qMax(GearParamLimits::kPinionMin, params->teeth / 2);
    if (params->pinionTeeth > pinionCap) {
        const int before = params->pinionTeeth;
        params->pinionTeeth = pinionCap;
        notes.append(QStringLiteral("从动轮齿数 %1 → %2（不应超过主动轮的一半）")
                         .arg(before)
                         .arg(pinionCap));
    }

    clampDouble(&params->module, GearParamLimits::kModuleMin, GearParamLimits::kModuleMax,
                QStringLiteral("模数"), &notes);
    clampDouble(&params->torque, GearParamLimits::kTorqueMin, GearParamLimits::kTorqueMax,
                QStringLiteral("扭矩"), &notes);
    clampDouble(&params->faceWidth, GearParamLimits::kFaceWidthMin,
                GearParamLimits::kFaceWidthMax, QStringLiteral("齿宽"), &notes);

    if (params->material.trimmed().isEmpty()) {
        params->material = QStringLiteral("20CrMnTi");
        notes.append(QStringLiteral("材料为空 → 20CrMnTi"));
    }
    return notes;
}
