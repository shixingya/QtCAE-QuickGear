#include "GearParams.h"

#include <QPair>
#include <QVector>
#include <QtGlobal>

namespace {

/** 材料表：{ 界面上的标准写法, 许用应力 MPa } */
struct MaterialEntry
{
    const char *name;
    double      allowable;
};

constexpr MaterialEntry kMaterials[] = {
    { "20CrMnTi 渗碳淬火", 400.0 },
    { "40Cr 调质",       360.0 },
    { "45 钢调质",       300.0 },
    { "尼龙 PA66",         60.0 },
};

/**
 * 早期版本只存了裸牌号（如 "20CrMnTi"），手改的文件也可能写成 "40Cr"。
 * 按牌号前缀认一次，然后一律换成标准写法——否则下拉里选不中、界面上会留着上一个材料。
 */
QString byBrand(const QString &raw)
{
    if (raw.contains(QStringLiteral("20CrMnTi"), Qt::CaseInsensitive))
        return QStringLiteral("20CrMnTi 渗碳淬火");
    if (raw.contains(QStringLiteral("40Cr"), Qt::CaseInsensitive))
        return QStringLiteral("40Cr 调质");
    if (raw.contains(QStringLiteral("45"), Qt::CaseInsensitive))
        return QStringLiteral("45 钢调质");
    if (raw.contains(QStringLiteral("尼龙"), Qt::CaseInsensitive)
        || raw.contains(QStringLiteral("PA66"), Qt::CaseInsensitive))
        return QStringLiteral("尼龙 PA66");
    return QString();
}

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

QStringList GearParamLimits::materials()
{
    QStringList out;
    out.reserve(int(sizeof(kMaterials) / sizeof(kMaterials[0])));
    for (const MaterialEntry &entry : kMaterials)
        out.append(QString::fromUtf8(entry.name));
    return out;
}

QString GearParamLimits::defaultMaterial()
{
    return QString::fromUtf8(kMaterials[0].name);
}

QString GearParamLimits::canonicalMaterial(const QString &raw)
{
    const QString trimmed = raw.trimmed();
    if (trimmed.isEmpty())
        return QString();
    for (const MaterialEntry &entry : kMaterials) {
        if (trimmed.compare(QString::fromUtf8(entry.name), Qt::CaseInsensitive) == 0)
            return QString::fromUtf8(entry.name);
    }
    // 标准写法里可能被手改掉空格/括号：换一种取法再试
    const QString brand = byBrand(trimmed);
    if (!brand.isEmpty())
        return brand;
    return QString();
}

double GearParamLimits::allowableStressMPa(const QString &raw)
{
    const QString name = canonicalMaterial(raw);
    for (const MaterialEntry &entry : kMaterials) {
        if (name == QString::fromUtf8(entry.name))
            return entry.allowable;
    }
    return kUnknownAllowable;
}

QStringList clampToLimits(GearParams *params)
{
    QStringList notes;
    if (!params)
        return notes;

    clampInt(&params->teeth, GearParamLimits::kTeethMin, GearParamLimits::kTeethMax,
             QStringLiteral("主动轮齿数"), &notes);
    clampInt(&params->pinionTeeth, GearParamLimits::kPinionMin, GearParamLimits::kPinionMax,
             QStringLiteral("从动轮齿数"), &notes);
    // 这里曾再按 z₁/2 夹一道：读别人的工程时，z₂ 会被就地改小，还留着一条
    // “不应超过主动轮的一半”的记录。可 z₂ > z₁ 是增速传动，属于合法工况；
    // 区间只认 kPinionMin..kPinionMax（界面上也是这一份）。真要提醒就写在日志里，
    // 不动文件里的数——读一次就改一次，用户再保存，原值就没了。

    clampDouble(&params->module, GearParamLimits::kModuleMin, GearParamLimits::kModuleMax,
                QStringLiteral("模数"), &notes);
    clampDouble(&params->torque, GearParamLimits::kTorqueMin, GearParamLimits::kTorqueMax,
                QStringLiteral("扭矩"), &notes);
    clampDouble(&params->faceWidth, GearParamLimits::kFaceWidthMin,
                GearParamLimits::kFaceWidthMax, QStringLiteral("齿宽"), &notes);

    const QString material = GearParamLimits::canonicalMaterial(params->material);
    if (material.isEmpty()) {
        const QString before = params->material;
        params->material = GearParamLimits::defaultMaterial();
        notes.append(before.trimmed().isEmpty()
                         ? QStringLiteral("材料为空 → %1").arg(params->material)
                         : QStringLiteral("材料 \"%1\" 不在可选列表 → %2")
                               .arg(before, params->material));
    } else {
        params->material = material;
    }
    return notes;
}
