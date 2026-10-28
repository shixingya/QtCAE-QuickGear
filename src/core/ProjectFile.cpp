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
    out.params    = paramsFromJson(root.value(QStringLiteral("params")).toObject());
    out.note      = root.value(QStringLiteral("note")).toString();
    out.meshPath  = root.value(QStringLiteral("meshPath")).toString();
    out.savedAt   = root.value(QStringLiteral("savedAt")).toString();
    out.hasResult = root.value(QStringLiteral("hasResult")).toBool();
    if (out.hasResult) {
        out.result = resultFromJson(root.value(QStringLiteral("result")).toObject());
        out.hasResult = out.result.ok;   // 存了但 ok=false 的结果视作没有结果
    }

    *data = out;
    return true;
}
