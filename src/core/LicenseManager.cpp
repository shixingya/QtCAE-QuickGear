#include "LicenseManager.h"

#include "core/Ed25519.h"

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

QString LicenseManager::normalizeMachineCode(const QString &raw)
{
    QString code = raw.trimmed().toUpper();
    code.remove(QLatin1Char(' '));
    code.remove(QLatin1Char('\t'));

    // 客户回信里常见三种写法：带连字符的、连写的、从截图里 OCR 出空格的。
    // 连写且长度刚好 32 位时按 4 位一组补齐，否则原样返回（让校验去报错）。
    if (!code.contains(QLatin1Char('-')) && code.size() == 32)
        return formatCode(code);
    return code;
}

bool LicenseManager::machineCodeLooksValid(const QString &raw)
{
    const QString code = normalizeMachineCode(raw);
    if (code.size() != 39)
        return false;
    for (int i = 0; i < code.size(); ++i) {
        const QChar ch = code.at(i);
        if ((i + 1) % 5 == 0) {
            if (ch != QLatin1Char('-'))
                return false;
        } else if (!(ch.isDigit() || (ch >= QLatin1Char('A') && ch <= QLatin1Char('F')))) {
            return false;
        }
    }
    return true;
}

QString LicenseManager::canonicalString(const QString &machine, const QString &licensee,
                                       const QString &edition, const QDate &issued,
                                       const QDate &expire)
{
    // 约定：机器码统一大写并补齐连字符、日期统一 ISO（yyyy-MM-dd）、空日期写作空串。
    // 签发工具与客户端共用本函数，保证两边拼串规则一致。
    return QStringList{
               normalizeMachineCode(machine),
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

QByteArray LicenseManager::signingPublicKey()
{
#ifdef QTG_LICENSE_PUBKEY
    static const QByteArray pub = QByteArray::fromBase64(QByteArray(QTG_LICENSE_PUBKEY));
    return pub.size() == 32 ? pub : QByteArray();
#else
    return QByteArray();
#endif
}

bool LicenseManager::verifyEd25519(const QString &canonical, const QByteArray &signature64)
{
    return verifyEd25519With(signingPublicKey(), canonical, signature64);
}

bool LicenseManager::verifyEd25519With(const QByteArray &publicKey, const QString &canonical,
                                       const QByteArray &signature64)
{
    if (publicKey.size() != 32 || signature64.size() != 64)
        return false;
    return Ed25519::verify(canonical.toUtf8(), signature64, publicKey);
}

QString LicenseManager::algName(const QString &alg)
{
    if (alg == QLatin1String("ed25519"))
        return QStringLiteral("Ed25519 公钥签名");
    if (alg == QLatin1String("hmac"))
        return QStringLiteral("HMAC-SHA256 共享密钥（旧格式，建议换成 Ed25519）");
    return alg.isEmpty() ? QStringLiteral("未知") : alg;
}

LicenseInfo LicenseManager::parseFile(const QString &path, bool checkMachine)
{
    LicenseInfo info;
    info.machineCode = machineCode();

    QFile file(path);
    if (!file.exists()) {
        info.state    = LicenseState::Missing;
        info.edition  = QStringLiteral("trial");
        info.message  = QStringLiteral("未安装授权文件，当前为试用状态");
        return info;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        info.state   = LicenseState::Unreadable;
        info.message = QStringLiteral("授权文件无法读取：%1").arg(file.errorString());
        return info;
    }

    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    file.close();

    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        info.state   = LicenseState::Unreadable;
        info.message = QStringLiteral("授权文件格式错误：%1").arg(parseError.errorString());
        return info;
    }

    const QJsonObject obj = doc.object();
    info.licensee = obj.value(QStringLiteral("licensee")).toString();
    info.edition  = obj.value(QStringLiteral("edition")).toString(QStringLiteral("pro"));
    info.issued   = QDate::fromString(obj.value(QStringLiteral("issued")).toString(), Qt::ISODate);
    info.expire   = QDate::fromString(obj.value(QStringLiteral("expire")).toString(), Qt::ISODate);
    info.daysLeft = info.expire.isValid() ? QDate::currentDate().daysTo(info.expire) : -1;

    info.boundMachine = normalizeMachineCode(obj.value(QStringLiteral("machine")).toString());
    if (info.boundMachine.isEmpty()) {
        info.state   = LicenseState::Unreadable;
        info.message = QStringLiteral("授权文件缺少 machine 字段");
        return info;
    }

    // 先验签再判适用性：签名都不过的文件不该继续被当成“只是机器不匹配”
    const QString canonical = canonicalString(info.boundMachine, info.licensee, info.edition,
                                              info.issued, info.expire);
    const QByteArray sig = obj.value(QStringLiteral("sig")).toString().toLatin1();
    if (sig.isEmpty()) {
        info.state   = LicenseState::BadSignature;
        info.message = QStringLiteral("授权文件缺少签名（sig），无法确认来源");
        return info;
    }

    // v1 文件没有 alg 字段，按 HMAC 处理（旧的都这样签的）
    info.alg = obj.value(QStringLiteral("alg")).toString().trimmed().toLower();
    if (info.alg.isEmpty())
        info.alg = QStringLiteral("hmac");

    if (info.alg == QLatin1String("ed25519")) {
        if (signingPublicKey().isEmpty()) {
            info.state   = LicenseState::BadSignature;
            info.message = QStringLiteral("这份授权用 Ed25519 签发，但当前构建未内置对应公钥（"
                                          "请使用正式构建的客户端）");
            return info;
        }
        if (!verifyEd25519(canonical, QByteArray::fromBase64(sig))) {
            info.state   = LicenseState::BadSignature;
            info.message = QStringLiteral("签名校验失败：授权文件被修改，或不是由本工具签发");
            return info;
        }
    } else if (info.alg == QLatin1String("hmac")) {
        if (!verifySignature(canonical, sig)) {
            info.state   = LicenseState::BadSignature;
            info.message = QStringLiteral("签名校验失败：授权文件被修改，或不是由签发工具生成");
            return info;
        }
    } else {
        info.state   = LicenseState::BadSignature;
        info.message = QStringLiteral("不认识的签名算法：%1").arg(info.alg);
        return info;
    }
    info.signatureChecked = true;

    if (checkMachine && info.boundMachine != info.machineCode) {
        info.state   = LicenseState::ForeignMachine;
        info.message = QStringLiteral("授权与本机不匹配：授权码 %1，本机 %2")
                           .arg(info.boundMachine, info.machineCode);
        return info;
    }
    if (info.expire.isValid() && info.expire < QDate::currentDate()) {
        info.state   = LicenseState::Expired;
        info.message = QStringLiteral("授权已于 %1 到期，请联系续期")
                           .arg(info.expire.toString(Qt::ISODate));
        return info;
    }

    info.state   = LicenseState::Valid;
    info.valid   = true;
    info.message = info.expiringSoon()
                       ? QStringLiteral("授权有效，%1").arg(expiryText(info))
                       : QStringLiteral("授权有效（签名校验通过）");
    return info;
}

LicenseInfo LicenseManager::loadFromFile(const QString &path) const
{
    return parseFile(path, true);
}

QString LicenseManager::stateName(LicenseState state)
{
    switch (state) {
    case LicenseState::Missing:        return QStringLiteral("MISSING");
    case LicenseState::Unreadable:     return QStringLiteral("BADFILE");
    case LicenseState::BadSignature:   return QStringLiteral("BADSIG");
    case LicenseState::ForeignMachine: return QStringLiteral("FOREIGN");
    case LicenseState::Expired:        return QStringLiteral("EXPIRED");
    case LicenseState::Valid:          return QStringLiteral("VALID");
    }
    return QStringLiteral("UNKNOWN");
}

QString LicenseManager::expiryText(const LicenseInfo &info)
{
    if (!info.expire.isValid())
        return QStringLiteral("永久有效");
    if (info.daysLeft > 0)
        return QStringLiteral("剩 %1 天").arg(info.daysLeft);
    if (info.daysLeft == 0)
        return QStringLiteral("今日到期");
    return QStringLiteral("已过期 %1 天").arg(-info.daysLeft);
}

bool LicenseManager::writeLicenseFile(const QString &path, const QString &machine,
                                      const QString &licensee, const QString &edition,
                                      const QDate &issued, const QDate &expire,
                                      QString *signatureBase64, QString *error,
                                      const QByteArray &signingSeed)
{
    const QString bound = normalizeMachineCode(machine);
    if (bound.isEmpty()) {
        if (error) *error = QStringLiteral("机器码为空");
        return false;
    }
    if (!machineCodeLooksValid(bound)) {
        if (error)
            *error = QStringLiteral("机器码格式不对（应为 8 组 4 位十六进制）：%1").arg(bound);
        return false;
    }
    if (licensee.trimmed().isEmpty()) {
        if (error) *error = QStringLiteral("授权对象为空");
        return false;
    }

    const QString canonical = canonicalString(bound, licensee, edition, issued, expire);

    QString sig;
    QString alg;
    if (signingSeed.isEmpty()) {
        // 兼容路径：旧的 HMAC。新签发不要走这里，密钥是编在客户端里的
        sig = QString::fromLatin1(signPayload(canonical).toBase64());
        alg = QStringLiteral("hmac");
    } else {
        QByteArray sig64;
        if (!Ed25519::sign(canonical.toUtf8(), signingSeed, &sig64, error))
            return false;
        sig = QString::fromLatin1(sig64.toBase64());
        alg = QStringLiteral("ed25519");
    }

    QJsonObject obj;
    obj.insert(QStringLiteral("v"), signingSeed.isEmpty() ? 1 : 2);
    obj.insert(QStringLiteral("alg"), alg);
    obj.insert(QStringLiteral("machine"), bound);
    obj.insert(QStringLiteral("licensee"), licensee.trimmed());
    obj.insert(QStringLiteral("edition"), edition.trimmed());
    obj.insert(QStringLiteral("issued"), issued.isValid() ? issued.toString(Qt::ISODate) : QString());
    obj.insert(QStringLiteral("expire"), expire.isValid() ? expire.toString(Qt::ISODate) : QString());
    obj.insert(QStringLiteral("sig"), sig);
    obj.insert(QStringLiteral("note"), QStringLiteral("QtCAE-QuickGear offline license"));

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error) *error = QStringLiteral("无法写入 %1：%2").arg(path, file.errorString());
        return false;
    }
    file.write(QJsonDocument(obj).toJson(QJsonDocument::Indented));
    file.close();

    if (signatureBase64) *signatureBase64 = sig;
    return true;
}

LicenseInfo LicenseManager::current() const
{
    return loadFromFile(defaultLicensePath());
}

bool LicenseManager::installFrom(const QString &srcPath, QString *error,
                                 const QString &targetPath) const
{
    QFile src(srcPath);
    if (!src.exists()) {
        if (error) *error = QStringLiteral("文件不存在：%1").arg(srcPath);
        return false;
    }

    const QString target = targetPath.isEmpty() ? defaultLicensePath() : targetPath;
    if (QFileInfo(srcPath).absoluteFilePath() == QFileInfo(target).absoluteFilePath())
        return true;

    // 先验后用：把一个签名对不上的文件装进去，等于顺手弄丢客户手上那张能用的授权
    const LicenseInfo incoming = parseFile(srcPath, true);
    if (incoming.state == LicenseState::BadSignature
        || incoming.state == LicenseState::ForeignMachine
        || incoming.state == LicenseState::Unreadable) {
        if (error) {
            *error = QStringLiteral("这份授权未通过校验（%1），原授权保持不动。\n%2")
                         .arg(stateName(incoming.state), incoming.message);
        }
        return false;
    }

    QDir().mkpath(QFileInfo(target).absolutePath());

    // 不许拿一张已过期的授权换掉手上还能用的那张：客户往往是错点了旧邮件里的附件
    const LicenseInfo existing = QFile::exists(target) ? parseFile(target, true) : LicenseInfo();
    if (incoming.state == LicenseState::Expired && existing.valid) {
        if (error) {
            *error = QStringLiteral("现有授权仍然有效（%1），不会被一份已过期的授权覆盖。")
                         .arg(expiryText(existing));
        }
        return false;
    }

    // 先落到临时文件再整体替换：原实现先删旧的再复制，复制一失败就两头落空
    const QString staging = target + QStringLiteral(".incoming");
    QFile::remove(staging);
    if (!src.copy(staging)) {
        if (error) *error = QStringLiteral("复制授权文件失败：%1").arg(src.errorString());
        return false;
    }

    const QString backup = target + QStringLiteral(".previous");
    QFile::remove(backup);
    const bool hadPrevious = QFile::exists(target);
    if (hadPrevious && !QFile::rename(target, backup)) {
        QFile::remove(staging);
        if (error) *error = QStringLiteral("无法替换原授权（文件可能正被占用）：%1").arg(target);
        return false;
    }
    if (!QFile::rename(staging, target)) {
        if (hadPrevious) QFile::rename(backup, target);   // 替换失败就把旧的放回去
        QFile::remove(staging);
        if (error) *error = QStringLiteral("写入授权文件失败，原授权已恢复：%1").arg(target);
        return false;
    }
    QFile::remove(backup);

    // 过期文件允许导入（客户手里可能还是上一份），但必须让调用方看见这句话
    if (incoming.state == LicenseState::Expired && error)
        *error = QStringLiteral("授权已导入，但它已于 %1 到期")
                     .arg(incoming.expire.toString(QStringLiteral("yyyy-MM-dd")));
    return true;
}
