#include "core/LicenseManager.h"

#include <QCoreApplication>
#include <QDate>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>

/**
 * qglicense —— QtCAE-QuickGear 授权签发 / 校验工具（开发者自用，控制台程序）。
 *
 *   qglicense machine                              打印本机机器码
 *   qglicense issue --licensee <name> [--days 365] 签发授权（默认绑本机）
 *   qglicense verify [file]                        校验授权（默认校验已安装的那份）
 *   qglicense install <file>                       把授权安装到客户端读取路径
 *
 * 签名：HMAC-SHA256(编译期注入的密钥, "machine|licensee|edition|issued|expire")，
 * 与客户端 LicenseManager 共用同一套代码，因此两边不会出现拼串不一致。
 *
 * 输出刻意使用 ASCII 键值对（VALID=YES 之类），避免控制台代码页不一致导致乱码，
 * 也便于脚本、CI 里直接 grep。
 */

namespace {

QTextStream &out()
{
    static QTextStream stream(stdout);
    return stream;
}

QTextStream &err()
{
    static QTextStream stream(stderr);
    return stream;
}

void printUsage()
{
    out() << "qglicense - QtCAE-QuickGear license issuer\n\n"
          << "Usage:\n"
          << "  qglicense machine\n"
          << "  qglicense issue --licensee <name> [--edition pro] [--days 365]\n"
          << "                 [--expire 2027-12-31] [--issued 2026-10-28]\n"
          << "                 [--machine <CODE>] [--out <file>]\n"
          << "  qglicense verify [<file>]\n"
          << "  qglicense install <file>\n\n"
          << "Notes:\n"
          << "  * issue 默认绑定本机机器码，也可用 --machine 给别的机器签发。\n"
          << "  * 授权文件写到当前目录，安装用 install 复制到客户端读取位置。\n"
          << "  * 签名密钥在编译期注入（CMake: QTG_LICENSE_SECRET），签发端与客户端必须一致。\n";
    out().flush();
}

/** 解析 --key value 形式的参数；位置参数收进 positional */
QHash<QString, QString> parseOptions(const QStringList &args, QStringList *positional)
{
    QHash<QString, QString> opts;
    for (int i = 0; i < args.size(); ++i) {
        const QString &token = args.at(i);
        if (!token.startsWith(QLatin1String("--"))) {
            if (positional)
                positional->append(token);
            continue;
        }
        const QString key = token.mid(2);
        if (i + 1 < args.size() && !args.at(i + 1).startsWith(QLatin1String("--"))) {
            opts.insert(key, args.at(++i));
        } else {
            opts.insert(key, QString());   // 只认开关，不留值
        }
    }
    return opts;
}

int doMachine()
{
    out() << "MACHINE=" << LicenseManager::machineCode() << "\n";
    out().flush();
    return 0;
}

int doIssue(const QStringList &args)
{
    const QHash<QString, QString> opts = parseOptions(args, nullptr);

    const QString licensee = opts.value(QStringLiteral("licensee")).trimmed();
    if (licensee.isEmpty()) {
        err() << "ERROR=missing --licensee\n";
        err().flush();
        return 2;
    }

    const QString machine = opts.value(QStringLiteral("machine"),
                                       LicenseManager::machineCode()).trimmed().toUpper();
    const QString edition = opts.value(QStringLiteral("edition"), QStringLiteral("pro")).trimmed();

    // --issued 用于补开历史授权或本地测试（例如验一下客户端对过期授权的反应）
    QDate issued = QDate::currentDate();
    if (opts.contains(QStringLiteral("issued"))) {
        issued = QDate::fromString(opts.value(QStringLiteral("issued")), Qt::ISODate);
        if (!issued.isValid()) {
            err() << "ERROR=invalid --issued, expected yyyy-MM-dd\n";
            err().flush();
            return 2;
        }
    }

    QDate expire;
    if (opts.contains(QStringLiteral("expire"))) {
        expire = QDate::fromString(opts.value(QStringLiteral("expire")), Qt::ISODate);
        if (!expire.isValid()) {
            err() << "ERROR=invalid --expire, expected yyyy-MM-dd\n";
            err().flush();
            return 2;
        }
    } else {
        bool ok      = false;
        const int days = opts.value(QStringLiteral("days"), QStringLiteral("365")).toInt(&ok);
        if (!ok || days <= 0) {
            err() << "ERROR=invalid --days\n";
            err().flush();
            return 2;
        }
        expire = issued.addDays(days);
    }

    if (expire < issued) {
        err() << "ERROR=expire date is before the issue date\n";
        err().flush();
        return 2;
    }

    // 与客户端完全相同的规范化串与签名算法
    const QString canonical = LicenseManager::canonicalString(machine, licensee, edition, issued, expire);
    const QString sig = QString::fromLatin1(LicenseManager::signPayload(canonical).toBase64());

    QJsonObject obj;
    obj.insert(QStringLiteral("v"), 1);
    obj.insert(QStringLiteral("machine"), machine);
    obj.insert(QStringLiteral("licensee"), licensee);
    obj.insert(QStringLiteral("edition"), edition);
    obj.insert(QStringLiteral("issued"), issued.toString(Qt::ISODate));
    obj.insert(QStringLiteral("expire"), expire.toString(Qt::ISODate));
    obj.insert(QStringLiteral("sig"), sig);
    obj.insert(QStringLiteral("note"), QStringLiteral("QtCAE-QuickGear offline license"));

    QString outPath = opts.value(QStringLiteral("out")).trimmed();
    if (outPath.isEmpty()) {
        outPath = QDir::current().filePath(
            QStringLiteral("license-%1-%2.qglic").arg(machine.left(9), expire.toString(Qt::ISODate)));
    } else if (!outPath.endsWith(QLatin1String(".qglic"), Qt::CaseInsensitive)) {
        outPath += QStringLiteral(".qglic");
    }

    QFile file(outPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        err() << "ERROR=cannot write " << outPath << ": " << file.errorString() << "\n";
        err().flush();
        return 3;
    }
    file.write(QJsonDocument(obj).toJson(QJsonDocument::Indented));
    file.close();

    out() << "ISSUED=" << QFileInfo(outPath).absoluteFilePath() << "\n"
          << "MACHINE=" << machine << "\n"
          << "LICENSEE=" << licensee << "\n"
          << "EDITION=" << edition << "\n"
          << "EXPIRES=" << expire.toString(Qt::ISODate) << "\n"
          << "SIG=" << sig << "\n";
    out().flush();
    return 0;
}

int doVerify(const QStringList &args)
{
    const QString path = args.isEmpty() ? LicenseManager::defaultLicensePath() : args.first();
    LicenseManager manager;
    const LicenseInfo info = manager.loadFromFile(path);

    out() << "FILE=" << QFileInfo(path).absoluteFilePath() << "\n"
          << "MACHINE=" << info.machineCode << "\n"
          << "VALID=" << (info.valid ? "YES" : "NO") << "\n"
          << "SIGNED=" << (info.signatureChecked ? "YES" : "NO") << "\n"
          << "LICENSEE=" << info.licensee << "\n"
          << "EDITION=" << info.edition << "\n"
          << "EXPIRES=" << (info.expire.isValid() ? info.expire.toString(Qt::ISODate) : QStringLiteral("-")) << "\n"
          << "MESSAGE=" << info.message << "\n";
    out().flush();
    return info.valid ? 0 : 1;
}

int doInstall(const QStringList &args)
{
    if (args.isEmpty()) {
        err() << "ERROR=install needs a file path\n";
        err().flush();
        return 2;
    }

    LicenseManager manager;
    QString error;
    if (!manager.installFrom(args.first(), &error)) {
        err() << "ERROR=" << error << "\n";
        err().flush();
        return 3;
    }

    const LicenseInfo info = manager.current();
    out() << "INSTALLED=" << LicenseManager::defaultLicensePath() << "\n"
          << "VALID=" << (info.valid ? "YES" : "NO") << "\n"
          << "MESSAGE=" << info.message << "\n";
    out().flush();
    return info.valid ? 0 : 1;
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    // 必须与客户端保持一致，否则 QStandardPaths 推出的授权目录不同
    QCoreApplication::setApplicationName(QStringLiteral("QtCAE-QuickGear"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.1.0"));
    QCoreApplication::setOrganizationName(QStringLiteral("shixingya"));

    QStringList args = QCoreApplication::arguments();
    args.removeFirst();
    if (args.isEmpty()) {
        printUsage();
        return 1;
    }

    const QString command = args.takeFirst();
    if (command == QLatin1String("machine"))
        return doMachine();
    if (command == QLatin1String("issue"))
        return doIssue(args);
    if (command == QLatin1String("verify"))
        return doVerify(args);
    if (command == QLatin1String("install"))
        return doInstall(args);
    if (command == QLatin1String("-h") || command == QLatin1String("--help")
        || command == QLatin1String("help")) {
        printUsage();
        return 0;
    }

    err() << "ERROR=unknown command: " << command << "\n";
    err().flush();
    printUsage();
    return 1;
}
