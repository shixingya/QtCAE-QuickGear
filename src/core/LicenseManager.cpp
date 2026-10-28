#include "LicenseManager.h"

#include <QCryptographicHash>
#include <QDate>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageAuthenticationCode>
#include <QSettings>
#include <QStandardPaths>
#include <QSysInfo>

namespace {

/** 从注册表读 Windows MachineGuid；非 Windows 或读取失败时返回空 */
QString windowsMachineGuid()
{
#if defined(Q_OS_WIN)
    QSettings reg(QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Cryptography"),
                  QSettings::NativeFormat);
    const QString guid = reg.value(QStringLiteral("MachineGuid")).toString().trimmed();
    if (!guid.isEmpty())
        return guid;
#endif
    return QString();
}

/**
 * 编译期注入的签名密钥（CMake: -DQTG_LICENSE_SECRET=...）。
 * 未注入时使用一个显式无意义的占位值，同时让签发/验签依然自洽。
 */
QByteArray licenseSecret()
{
#ifdef QTG_LICENSE_SECRET
    return QByteArray(QTG_LICENSE_SECRET);
#else
    return QByteArrayLiteral("QTG-NO-SECRET-COMPILED");
#endif
}

} // namespace

QByteArray LicenseManager::rawMachineId()
{
    // 多源组合：任一来源变化都会得到不同机器码（换主板/重装系统会变，属预期行为）
    QStringList parts;
    parts << QString::fromUtf8(QSysInfo::machineUniqueId());   // Windows: MachineGuid 派生
    parts << QSysInfo::prettyProductName();
    parts << QSysInfo::currentCpuArchitecture();
    parts << windowsMachineGuid();

    const QByteArray joined = parts.join(QLatin1Char('|')).toUtf8();
    return QCryptographicHash::hash(joined, QCryptographicHash::Sha256).toHex().toUpper();
}

QString LicenseManager::formatCode(const QString &hex)
{
    QString out;
    out.reserve(hex.size() + hex.size() / 4);
    for (int i = 0; i < hex.size(); ++i) {
        if (i > 0 && i % 4 == 0)
            out.append(QLatin1Char('-'));
        out.append(hex.at(i));
    }
    return out;
}

QString LicenseManager::machineCode()
{
    static const QString cached = formatCode(QString::fromLatin1(rawMachineId().left(32)));
    return cached;
}

QString LicenseManager::defaultLicensePath()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);
    return dir + QStringLiteral("/license.qglic");
}

QString LicenseManager::canonicalString(const QString &machine, const QString &licensee,
                                       const QString &edition, const QDate &issued,
                                       const QDate &expire)
{
    // 约定：机器码统一大写、日期统一 ISO（yyyy-MM-dd）、空日期写作空串。
    // 签发工具与客户端共用本函数，保证两边拼串规则一致。
    return QStringList{
               machine.trimmed().toUpper(),
               licensee.trimmed(),
               edition.trimmed(),
               issued.isValid() ? issued.toString(Qt::ISODate) : QString(),
               expire.isValid() ? expire.toString(Qt::ISODate) : QString(),
           }
        .join(QLatin1Char('|'));
}

QByteArray LicenseManager::signPayload(const QString &canonical)
{
    QMessageAuthenticationCode mac(QCryptographicHash::Sha256, licenseSecret());
    mac.addData(canonical.toUtf8());
    return mac.result();
}

bool LicenseManager::verifySignature(const QString &canonical, const QByteArray &signatureBase64)
{
    const QByteArray expected = signPayload(canonical);
    const QByteArray given    = QByteArray::fromBase64(signatureBase64.trimmed());
    if (given.size() != expected.size())
        return false;

    // 常时间比较，避免按字节提前返回泄露信息
    unsigned char diff = 0;
    for (int i = 0; i < expected.size(); ++i)
        diff |= static_cast<unsigned char>(expected.at(i) ^ given.at(i));
    return diff == 0;
}

LicenseInfo LicenseManager::loadFromFile(const QString &path) const
{
    LicenseInfo info;
    info.machineCode = machineCode();

    QFile file(path);
    if (!file.exists()) {
        info.edition = QStringLiteral("trial");
        info.message = QStringLiteral("未安装授权文件，当前为试用状态");
        return info;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        info.message = QStringLiteral("授权文件无法读取：%1").arg(file.errorString());
        return info;
    }

    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    file.close();

    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        info.message = QStringLiteral("授权文件格式错误：%1").arg(parseError.errorString());
        return info;
    }

    const QJsonObject obj = doc.object();
    info.licensee = obj.value(QStringLiteral("licensee")).toString();
    info.edition  = obj.value(QStringLiteral("edition")).toString(QStringLiteral("pro"));
    info.issued   = QDate::fromString(obj.value(QStringLiteral("issued")).toString(), Qt::ISODate);
    info.expire   = QDate::fromString(obj.value(QStringLiteral("expire")).toString(), Qt::ISODate);

    const QString boundMachine = obj.value(QStringLiteral("machine")).toString().toUpper();
    if (boundMachine.isEmpty()) {
        info.message = QStringLiteral("授权文件缺少 machine 字段");
        return info;
    }
    if (boundMachine != info.machineCode) {
        info.message = QStringLiteral("授权与本机不匹配：授权码 %1，本机 %2")
                           .arg(boundMachine, info.machineCode);
        return info;
    }
    if (info.expire.isValid() && info.expire < QDate::currentDate()) {
        info.message = QStringLiteral("授权已于 %1 到期").arg(info.expire.toString(Qt::ISODate));
        return info;
    }

    // 验签：签名覆盖 machine|licensee|edition|issued|expire，任何字段被改动都会对不上
    const QString canonical = canonicalString(boundMachine, info.licensee, info.edition,
                                              info.issued, info.expire);
    const QByteArray sig = obj.value(QStringLiteral("sig")).toString().toLatin1();
    if (sig.isEmpty()) {
        info.message = QStringLiteral("授权文件缺少签名（sig），无法确认来源");
        return info;
    }
    if (!verifySignature(canonical, sig)) {
        info.message = QStringLiteral("签名校验失败：授权文件被修改，或不是由签发工具生成");
        return info;
    }

    info.signatureChecked = true;
    info.valid            = true;
    info.message          = QStringLiteral("授权有效（签名校验通过）");
    return info;
}

LicenseInfo LicenseManager::current() const
{
    return loadFromFile(defaultLicensePath());
}

bool LicenseManager::installFrom(const QString &srcPath, QString *error) const
{
    QFile src(srcPath);
    if (!src.exists()) {
        if (error) *error = QStringLiteral("文件不存在：%1").arg(srcPath);
        return false;
    }

    const QString target = defaultLicensePath();
    if (QFileInfo(srcPath).absoluteFilePath() == QFileInfo(target).absoluteFilePath())
        return true;

    QFile::remove(target);
    if (!src.copy(target)) {
        if (error) *error = QStringLiteral("复制授权文件失败：%1").arg(src.errorString());
        return false;
    }
    return true;
}
