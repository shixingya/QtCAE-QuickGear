#include "core/LicenseBatch.h"
#include "core/LicenseManager.h"

#include <QCoreApplication>
#include <QDate>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QTextStream>
#include <QVector>

/**
 * qglicense —— QtCAE-QuickGear 授权签发 / 审计工具（开发者自用，控制台程序）。
 *
 *   qglicense machine                              打印本机机器码
 *   qglicense issue --licensee <name> [--days 365] 签发单张授权（默认绑本机）
 *   qglicense batch --in <台账.csv> [--out <目录>] 批量签发 + 出一张交付单
 *   qglicense audit <目录|文件…> [--days 30]        扫描已签授权，列出该续费的客户
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
          << "  qglicense batch --in <ledger.csv> [--out <dir>] [--dry-run]\n"
          << "  qglicense audit <dir|file>... [--days 30] [--csv <out.csv>]\n"
          << "  qglicense verify [<file>]\n"
          << "  qglicense install <file>\n\n"
          << "Ledger (CSV, UTF-8): machine,licensee,edition,days,expire\n"
          << "  * header optional; '#' starts a comment; 'days' and 'expire' are either/or.\n"
          << "  * one bad row does not stop the batch: it is reported and the rest still issue.\n\n"
          << "Notes:\n"
          << "  * issue defaults to this machine; use --machine to license another host.\n"
          << "  * files go to the current directory (issue) or --out (batch, default ./issued).\n"
          << "  * audit skips the machine binding check: customer codes never match yours.\n"
          << "  * signing key is injected at build time (CMake: QTG_LICENSE_SECRET).\n\n"
          << "Exit codes: 0 ok, 1 usage/verify failed, 2 bad argument,\n"
          << "            3 write failed, 4 issued/found items needing attention,\n"
          << "            5 audit report (--csv) could not be written.\n";
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

    // 与客户端完全相同的规范化串与签名算法，已经封装在 writeLicenseFile 里
    QString outPath = opts.value(QStringLiteral("out")).trimmed();
    if (outPath.isEmpty()) {
        outPath = QDir::current().filePath(
            QStringLiteral("license-%1-%2.qglic").arg(machine.left(9), expire.toString(Qt::ISODate)));
    } else if (!outPath.endsWith(QLatin1String(".qglic"), Qt::CaseInsensitive)) {
        outPath += QStringLiteral(".qglic");
    }

    QString sig;
    QString error;
    if (!LicenseManager::writeLicenseFile(outPath, machine, licensee, edition, issued, expire,
                                          &sig, &error)) {
        err() << "ERROR=" << error << "\n";
        err().flush();
        return 3;
    }

    out() << "ISSUED=" << QFileInfo(outPath).absoluteFilePath() << "\n"
          << "MACHINE=" << LicenseManager::normalizeMachineCode(machine) << "\n"
          << "LICENSEE=" << licensee << "\n"
          << "EDITION=" << edition << "\n"
          << "EXPIRES=" << expire.toString(Qt::ISODate) << "\n"
          << "SIG=" << sig << "\n";
    out().flush();
    return 0;
}

/**
 * batch：一张台账 → 一批授权文件 + 一张交付单。
 *
 * 关键行为：坏行只影响它自己。第 7 行机器码抄错时，前 6 家与后 20 家照发，
 * 交付单上第 7 行是 FAIL 加原因——这比“整批重来”少一次人工核对。
 */
int doBatch(const QStringList &args)
{
    const QHash<QString, QString> opts = parseOptions(args, nullptr);

    const QString ledgerPath = opts.value(QStringLiteral("in")).trimmed();
    if (ledgerPath.isEmpty()) {
        err() << "ERROR=missing --in <ledger.csv>\n";
        err().flush();
        return 2;
    }

    QFile ledger(ledgerPath);
    if (!ledger.open(QIODevice::ReadOnly)) {
        err() << "ERROR=cannot read " << ledgerPath << ": " << ledger.errorString() << "\n";
        err().flush();
        return 2;
    }
    const QByteArray content = ledger.readAll();
    ledger.close();

    const bool    dryRun = opts.contains(QStringLiteral("dry-run"));
    QString       outDir = opts.value(QStringLiteral("out")).trimmed();
    if (outDir.isEmpty())
        outDir = QDir::current().filePath(QStringLiteral("issued"));

    QStringList rowErrors;
    const QVector<BatchOrder> orders = LicenseBatch::parseLedger(content, &rowErrors);
    for (const QString &e : rowErrors)
        err() << "ROW_ERROR=" << e << "\n";

    if (orders.isEmpty()) {
        err() << "ERROR=no issuable rows in " << ledgerPath << "\n";
        err().flush();
        out().flush();
        return 4;
    }

    QStringList issueErrors;
    const QVector<BatchResult> results =
        LicenseBatch::issueAll(orders, outDir, dryRun, &issueErrors);
    for (const QString &e : issueErrors)
        err() << "ERROR=" << e << "\n";

    int okCount = 0;
    int failCount = 0;
    for (const BatchResult &result : results) {
        if (result.ok()) {
            ++okCount;
            // dry-run 不能报 ISSUED：脚本里 grep 这个词就等于以为文件已经发出去了
            out() << (dryRun ? "PLANNED=" : "ISSUED=")
                  << QFileInfo(result.file).absoluteFilePath()
                  << " MACHINE=" << LicenseManager::normalizeMachineCode(result.order.machine)
                  << " LICENSEE=" << result.order.licensee
                  << " EXPIRES="
                  << (result.order.expire.isValid() ? result.order.expire.toString(Qt::ISODate)
                                                    : QStringLiteral("perpetual"))
                  << "\n";
        } else {
            ++failCount;
            err() << "FAILED=line " << result.order.line << " " << result.order.licensee
                  << " :: " << result.error << "\n";
        }
    }

    // 交付单已由 LicenseBatch::issueAll 跟在授权文件后面写好，这里只把它报出来
    if (!dryRun && okCount > 0) {
        const QString manifest = LicenseBatch::manifestPath(outDir);
        if (QFileInfo::exists(manifest))
            out() << "MANIFEST=" << QFileInfo(manifest).absoluteFilePath() << "\n";
    }

    out() << "SUMMARY=" << (dryRun ? "DRY-RUN" : "ISSUED")
          << " OK=" << okCount << " FAILED=" << failCount
          << " BAD_ROWS=" << rowErrors.size()
          << " OUT=" << QDir::toNativeSeparators(outDir) << "\n";
    out().flush();
    err().flush();
    return (failCount == 0 && rowErrors.isEmpty()) ? 0 : 4;
}

/**
 * audit：扫描已签的授权文件，把该续费的客户挑出来。
 *
 * 这里必须跳过机器码绑定校验（checkMachine = false）：客户的机器码本来就不是本机的，
 * 照常校验的话每张都是 FOREIGN，看不到到期日。验签仍然做，
 * 所以“该续费”与“文件被改过”能分开统计。
 */
int doAudit(const QStringList &args)
{
    QStringList                   positional;
    const QHash<QString, QString> opts = parseOptions(args, &positional);

    if (positional.isEmpty()) {
        err() << "ERROR=audit needs a directory or a .qglic file\n";
        err().flush();
        return 2;
    }

    int window = kExpiryWarnDays;
    if (opts.contains(QStringLiteral("days"))) {
        bool ok = false;
        window  = opts.value(QStringLiteral("days")).toInt(&ok);
        if (!ok || window < 0) {
            err() << "ERROR=invalid --days\n";
            err().flush();
            return 2;
        }
    }

    QStringList files;
    for (const QString &path : positional) {
        const QFileInfo info(path);
        if (info.isDir()) {
            QDirIterator it(path, QStringList{QStringLiteral("*.qglic")}, QDir::Files,
                            QDirIterator::Subdirectories);
            while (it.hasNext())
                files.append(it.next());
        } else if (info.exists()) {
            files.append(info.absoluteFilePath());
        } else {
            err() << "ERROR=no such path: " << path << "\n";
        }
    }
    files.removeDuplicates();
    files.sort();

    if (files.isEmpty()) {
        err() << "ERROR=no .qglic files found\n";
        err().flush();
        return 2;
    }

    int valid = 0;
    int expiring = 0;
    int expired = 0;
    int broken = 0;

    QStringList csvLines;
    csvLines << QStringLiteral("machine,licensee,edition,issued,expire,days_left,state,file");

    // 任何字段都可能带逗号（客户名、路径），不能只管 licensee 一个
    const auto csvCell = [](const QString &value) {
        QString cell = value;
        cell.replace(QStringLiteral("\""), QStringLiteral("\"\""));
        if (cell.contains(QLatin1Char(',')) || cell.contains(QLatin1Char('"'))
            || cell.contains(QLatin1Char('\n')))
            cell = QLatin1Char('"') + cell + QLatin1Char('"');
        return cell;
    };

    for (const QString &file : files) {
        const LicenseInfo info = LicenseManager::parseFile(file, false);

        QString flag = QStringLiteral("OK");
        if (info.state == LicenseState::Valid) {
            if (info.expire.isValid() && info.daysLeft <= window) {
                ++expiring;
                flag = QStringLiteral("EXPIRING");
            } else {
                ++valid;
            }
        } else if (info.state == LicenseState::Expired) {
            ++expired;
            flag = QStringLiteral("EXPIRED");
        } else {
            ++broken;
            flag = QStringLiteral("BROKEN");
        }

        out() << "FILE=" << QDir::toNativeSeparators(file) << "\n"
              << "  MACHINE=" << info.boundMachine
              << " LICENSEE=" << (info.licensee.isEmpty() ? QStringLiteral("-") : info.licensee)
              << " EDITION=" << info.edition << "\n"
              << "  EXPIRES="
              << (info.expire.isValid() ? info.expire.toString(Qt::ISODate)
                                        : QStringLiteral("perpetual"))
              << " DAYS=" << (info.expire.isValid() ? info.daysLeft : -1)
              << " STATE=" << LicenseManager::stateName(info.state)
              << " FLAG=" << flag << "\n";

        // 中文公司名在控制台可能因代码页显示成乱码，CSV 是给 Excel 看的那一份
        csvLines << QStringList{
                        csvCell(info.boundMachine),
                        csvCell(info.licensee),
                        csvCell(info.edition),
                        csvCell(info.issued.isValid() ? info.issued.toString(Qt::ISODate) : QString()),
                        csvCell(info.expire.isValid() ? info.expire.toString(Qt::ISODate)
                                                     : QStringLiteral("perpetual")),
                        csvCell(info.expire.isValid() ? QString::number(info.daysLeft)
                                                     : QStringLiteral("-")),
                        csvCell(LicenseManager::stateName(info.state)),
                        csvCell(QDir::toNativeSeparators(file)),
                    }
                        .join(QLatin1Char(','));
    }

    bool csvFailed = false;
    const QString csvPath = opts.value(QStringLiteral("csv")).trimmed();
    if (!csvPath.isEmpty()) {
        QFile csv(csvPath);
        if (csv.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            csv.write(QByteArray("\xEF\xBB\xBF")
                      + (csvLines.join(QLatin1Char('\n')) + QLatin1Char('\n')).toUtf8());
            csv.close();
            out() << "CSV=" << QFileInfo(csvPath).absoluteFilePath() << "\n";
        } else {
            err() << "ERROR=cannot write " << csvPath << ": " << csv.errorString() << "\n";
            csvFailed = true;
        }
    }

    out() << "SUMMARY TOTAL=" << files.size() << " OK=" << valid
          << " EXPIRING=" << expiring << " EXPIRED=" << expired
          << " BROKEN=" << broken << " WINDOW=" << window << "\n";
    out().flush();
    err().flush();
    // 明细表没写出去就别报“一切正常”：对账少了那一张表就等于没记账
    if (csvFailed)
        return 5;
    return (expiring == 0 && expired == 0 && broken == 0) ? 0 : 4;
}

int doVerify(const QStringList &args)
{
    const QString path = args.isEmpty() ? LicenseManager::defaultLicensePath() : args.first();
    if (QFileInfo(path).isDir()) {
        // 目录不是一张授权：直接指着能一次看全整份名单的 audit
        err() << "ERROR=not a license file (it is a directory); try: qglicense audit \""
              << QDir::toNativeSeparators(path) << "\"\n";
        err().flush();
        return 2;
    }

    LicenseManager manager;
    const LicenseInfo info = manager.loadFromFile(path);

    out() << "FILE=" << QFileInfo(path).absoluteFilePath() << "\n"
          << "MACHINE=" << info.machineCode << "\n"
          << "VALID=" << (info.valid ? "YES" : "NO") << "\n"
          << "SIGNED=" << (info.signatureChecked ? "YES" : "NO") << "\n"
          << "STATE=" << LicenseManager::stateName(info.state) << "\n"
          << "LICENSEE=" << info.licensee << "\n"
          << "EDITION=" << info.edition << "\n"
          << "EXPIRES=" << (info.expire.isValid() ? info.expire.toString(Qt::ISODate) : QStringLiteral("-")) << "\n"
          << "DAYS_LEFT=" << (info.expire.isValid() ? QString::number(info.daysLeft) : QStringLiteral("-")) << "\n"
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
    // 装成功也可能有话说（例如这份授权已经过期），不能默默吞掉
    if (!error.isEmpty())
        out() << "WARN=" << error << "\n";

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
#ifdef QTG_VERSION
    QCoreApplication::setApplicationVersion(QStringLiteral(QTG_VERSION));
#else
    QCoreApplication::setApplicationVersion(QStringLiteral("0.3.0"));
#endif
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
    if (command == QLatin1String("batch"))
        return doBatch(args);
    if (command == QLatin1String("audit"))
        return doAudit(args);
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
