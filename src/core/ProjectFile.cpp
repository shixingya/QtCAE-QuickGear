#include "ProjectFile.h"

#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

namespace {

constexpr int    kFormatVersion = 2;
const char       kAppTag[]      = "QtCAE-QuickGear";
constexpr double kDefaultCeiling = 320.0;

QJsonObject paramsToJson(const GearParams &p)
{
    QJsonObject o;
    o.insert(QStringLiteral("teeth"), p.teeth);
    o.insert(QStringLiteral("pinionTeeth"), p.pinionTeeth);
    o.insert(QStringLiteral("module"), p.module);
    o.insert(QStringLiteral("torque"), p.torque);
    o.insert(QStringLiteral("faceWidth"), p.faceWidth);
    o.insert(QStringLiteral("material"), p.material);
    return o;
}

GearParams paramsFromJson(const QJsonObject &o)
{
    GearParams p;
    p.teeth       = o.value(QStringLiteral("teeth")).toInt(p.teeth);
    p.pinionTeeth = o.value(QStringLiteral("pinionTeeth")).toInt(p.pinionTeeth);
    p.module      = o.value(QStringLiteral("module")).toDouble(p.module);
    p.torque      = o.value(QStringLiteral("torque")).toDouble(p.torque);
    p.faceWidth   = o.value(QStringLiteral("faceWidth")).toDouble(p.faceWidth);
    p.material    = o.value(QStringLiteral("material")).toString(p.material);
    return p;
}

QJsonObject resultToJson(const SolveResult &r)
{
    QJsonObject o;
    o.insert(QStringLiteral("ok"), r.ok);
    o.insert(QStringLiteral("backend"), r.backend);
    o.insert(QStringLiteral("peakVonMises"), r.peakVonMises);
    o.insert(QStringLiteral("maxDeflection"), r.maxDeflection);
    o.insert(QStringLiteral("elements"), r.elements);
    o.insert(QStringLiteral("nodes"), r.nodes);
    o.insert(QStringLiteral("safetyFactor"), r.safetyFactor);
    o.insert(QStringLiteral("solveSeconds"), r.solveSeconds);
    o.insert(QStringLiteral("stressCeiling"), r.stressCeiling);

    QJsonArray teeth;
    for (double v : r.toothStress)
        teeth.append(v);
    o.insert(QStringLiteral("toothStress"), teeth);
    return o;
}

SolveResult resultFromJson(const QJsonObject &o)
{
    SolveResult r;
    r.ok            = o.value(QStringLiteral("ok")).toBool();
    r.backend       = o.value(QStringLiteral("backend")).toString();
    r.peakVonMises  = o.value(QStringLiteral("peakVonMises")).toDouble();
    r.maxDeflection = o.value(QStringLiteral("maxDeflection")).toDouble();
    r.elements      = o.value(QStringLiteral("elements")).toInt();
    r.nodes         = o.value(QStringLiteral("nodes")).toInt();
    r.safetyFactor  = o.value(QStringLiteral("safetyFactor")).toDouble();
    r.solveSeconds  = o.value(QStringLiteral("solveSeconds")).toDouble();
    r.stressCeiling = o.value(QStringLiteral("stressCeiling")).toDouble(kDefaultCeiling);

    const QJsonArray teeth = o.value(QStringLiteral("toothStress")).toArray();
    r.toothStress.reserve(teeth.size());
    for (const QJsonValue &v : teeth)
        r.toothStress.append(v.toDouble());
    return r;
}

/**
 * 工程文件里的“结果摘要”和“参数”不一定是一家人的：手改过、换过材料、旧版本写过。
 *
 * 云图和报告读的是同一份结果，却分别看两个字段：图上按逐齿数组上色，指标栏和
 * 结论文本用 peakVonMises、安全系数 n。两个数一旦对不上，用户看到的是一张自相矛盾
 * 的结论页：图上那两个齿红得发白，指标栏写着 σv=42、n=9.52、通过。
 *
 * 所以打开时按能自证的量修一道：峰值取逐齿最大值与自报峰值中较大的那个（对校核
 * 是保守方向），n 按“许用 / σv”重算。改了什么一律写进提醒，不静静改数。
 *
 * 返回 false 表示这份结果已经不能用了（逐齿数组与齿数对不上、峰值是 0），
 * 调用方要按“无结果”打开。
 */
bool reconcileResult(SolveResult *r, const GearParams &params, QStringList *notes)
{
    // 逐齿数组是云图上色的底数据：StressView 用 i % size 取色，个数对不上时
    // 热斑会在齿盘上重复出现（一半的齿红两次），看着像“有两个危险齿”。
    // 宁可按无结果打开，也不拿它编一张图——这个数补不出来。
    if (r->toothStress.size() != params.teeth) {
        notes->append(QStringLiteral("逐齿应力 %1 个，与主动轮 %2 齿对不上，已按无结果打开")
                          .arg(r->toothStress.size())
                          .arg(params.teeth));
        return false;
    }

    double toothPeak = 0.0;
    for (double v : r->toothStress)
        toothPeak = qMax(toothPeak, v);

    if (toothPeak > r->peakVonMises + 1e-9) {
        notes->append(QStringLiteral("结果峰值应力 %1 MPa 低于逐齿最大值 %2 MPa，已按 %2 计")
                          .arg(QString::number(r->peakVonMises, 'f', 1),
                               QString::number(toothPeak, 'f', 1)));
        r->peakVonMises = toothPeak;
    }

    if (!(r->peakVonMises > 0.0)) {
        notes->append(QStringLiteral("结果峰值应力为 0，已按无结果打开"));
        return false;
    }

    // n = 许用 / σv。文件里的 n 可能来自另一种材料、上一版的许用值表，或者根本没写；
    // 1% 是“显示两位小数看不出差别”的宽度，超过就重算
    const double limit    = GearParamLimits::allowableStressMPa(params.material);
    const double expected = limit / r->peakVonMises;
    if (!(qAbs(expected - r->safetyFactor) <= expected * 0.01)) {
        notes->append(QStringLiteral("结果安全系数 %1 与当前材料不符（许用 %2 / σv %3 = %4），已重算")
                          .arg(QString::number(r->safetyFactor, 'f', 2),
                               QString::number(limit, 'f', 0),
                               QString::number(r->peakVonMises, 'f', 1),
                               QString::number(expected, 'f', 2)));
        r->safetyFactor = expected;
    }

    return true;
}

} // namespace

QString projectFileFilter()
{
    return QStringLiteral("QuickGear 工程 (*.qgpj);;所有文件 (*.*)");
}

bool saveProject(const QString &path, const ProjectData &data, QString *error)
{
    QJsonObject root;
    root.insert(QStringLiteral("app"), QString::fromLatin1(kAppTag));
    root.insert(QStringLiteral("version"), kFormatVersion);
    root.insert(QStringLiteral("savedAt"), QDateTime::currentDateTime().toString(Qt::ISODate));
    root.insert(QStringLiteral("params"), paramsToJson(data.params));
    root.insert(QStringLiteral("note"), data.note);
    root.insert(QStringLiteral("meshPath"), data.meshPath);
    root.insert(QStringLiteral("hasResult"), data.hasResult);
    if (data.hasResult)
        root.insert(QStringLiteral("result"), resultToJson(data.result));

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        *error = QStringLiteral("无法写入：%1").arg(file.errorString());
        return false;
    }
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    if (!file.commit()) {
        *error = QStringLiteral("保存失败：%1").arg(file.errorString());
        return false;
    }
    return true;
}

bool loadProject(const QString &path, ProjectData *data, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("无法打开：%1").arg(file.errorString());
        return false;
    }

    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    file.close();

    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        *error = QStringLiteral("不是有效的工程文件（%1）").arg(parseError.errorString());
        return false;
    }

    const QJsonObject root = doc.object();
    if (root.value(QStringLiteral("app")).toString() != QString::fromLatin1(kAppTag)) {
        *error = QStringLiteral("工程文件不属于 QtCAE-QuickGear");
        return false;
    }

    const int version = root.value(QStringLiteral("version")).toInt();
    if (version > kFormatVersion) {
        *error = QStringLiteral("工程文件版本 %1 高于本程序支持的 %2，请升级客户端")
                     .arg(version)
                     .arg(kFormatVersion);
        return false;
    }

    ProjectData out;
    if (version <= 0) {
        // 没有版本号的只可能是早期手写/脚本生成的：按缺省字段读，但要说明白
        out.warnings.append(QStringLiteral("工程文件没有 version 字段，按当前格式宽松读取"));
    }
    out.params    = paramsFromJson(root.value(QStringLiteral("params")).toObject());
    // 工程文件是会给同事、客户、客服转发的：里面的数不能直接信任
    out.warnings.append(clampToLimits(&out.params));
    out.note      = root.value(QStringLiteral("note")).toString();
    out.meshPath  = root.value(QStringLiteral("meshPath")).toString();
    out.savedAt   = root.value(QStringLiteral("savedAt")).toString();
    out.hasResult = root.value(QStringLiteral("hasResult")).toBool();
    if (out.hasResult) {
        out.result = resultFromJson(root.value(QStringLiteral("result")).toObject());
        if (!out.result.ok) {
            out.hasResult = false;   // 存了但 ok=false 的结果视作没有结果
        } else if (out.result.toothStress.isEmpty()) {
            // 云图、逐齿色块、报告图全从这排数来。缺了它照样能显示出 σv 和 n，
            // 但那张“云图”会是一整片灰——宁可按无结果打开，也不要交一张假图。
            out.hasResult = false;
            out.warnings.append(QStringLiteral("上次结果缺逐齿应力，已按无结果打开"));
        } else {
            QStringList notes;
            const bool usable = reconcileResult(&out.result, out.params, &notes);
            out.warnings.append(notes);
            if (!usable)
                out.hasResult = false;
        }
    }

    *data = out;
    return true;
}
