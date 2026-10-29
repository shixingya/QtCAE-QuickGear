#include "core/Ed25519.h"
#include "core/LicenseBatch.h"
#include "core/LicenseManager.h"

#include <QCoreApplication>
#include <QDate>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QProcessEnvironment>
#include <QTextStream>
#include <QVector>

/**
 * qglicense —— QtCAE-QuickGear 授权签发 / 审计工具（开发者自用，控制台程序）。
 *
 *   qglicense keygen [--out <private.key>]              生成 Ed25519 密钥对（私钥只在签发机）
 *   qglicense issue --licensee <name> [--days 365] 签发单张授权（默认绑本机）
 *   qglicense batch --in <台账.csv> [--out <目录>] 批量签发 + 出一张交付单
 *   qglicense audit <目录|文件…> [--days 30]        扫描已签授权，列出该续费的客户
 *   qglicense verify [file]                        校验授权（默认校验已安装的那份）
 *   qglicense install <file>                       把授权安装到客户端读取路径
 *
 * 签名：Ed25519（客户端只内置公钥，拆开也签不出）；待签消息仍是
 * “machine|licensee|edition|issued|expire”，与客户端 LicenseManager 同源。
 * 私钥不在客户端里：默认从 --key 或环境变量 QTG_SIGNING_KEY 读；
 * 两者都没有时用编译期的演示密钥并明确警告（仅供开发）。
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
          << "  qglicense keygen [--out <private.key>]\n"
          << "  qglicense issue --licensee <name> [--edition pro] [--days 365]\n"
          << "                 [--expire 2027-12-31] [--issued 2026-10-28]\n"
          << "                 [--machine <CODE>] [--out <file>] [--key <file>]\n"
          << "  qglicense batch --in <ledger.csv> [--out <dir>] [--dry-run] [--key <file>]\n"
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
          << "  * signing key: --key <file> > env QTG_SIGNING_KEY > built-in demo seed.\n"
          << "  * the built-in seed is public; use keygen + -DQTG_LICENSE_PUBKEY for real builds.\n\n"
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

/** 密钥文件内容 → 32 字节种子：base64 / 64 位 hex / 原始 32 字节都认 */
QByteArray seedFromText(const QByteArray &raw, QString *error)
{
    const QByteArray text = raw.trimmed();
    const QByteArray b64 = QByteArray::fromBase64(text);
    if (b64.size() == 32)
        return b64;
    const QByteArray hex = QByteArray::fromHex(text);
    if (hex.size() == 32)
        return hex;
    if (raw.size() == 32)
        return raw;
    if (error) {
        *error = QStringLiteral("密钥里没有 32 字节种子（支持 base64 / 64 位 hex / 原始 32 字节）");
    }
    return QByteArray();
}

/**
 * 签名私钥：--key 文件 > 环境变量 QTG_SIGNING_KEY > 编译期演示种子。
 * 走到演示种子时一定要把警告带出去——那是公开值，签出来的授权拦不住任何人。
 */
QByteArray signingSeed(const QHash<QString, QString> &opts, QString *warning)
{
    const QString keyPath = opts.value(QStringLiteral("key")).trimmed();
    if (!keyPath.isEmpty()) {
        QFile key(keyPath);
        if (!key.open(QIODevice::ReadOnly)) {
            if (warning)
                *warning = QStringLiteral("无法读取私钥文件 %1：%2").arg(keyPath, key.errorString());
            return QByteArray();
        }
        return seedFromText(key.readAll(), warning);
    }

    const QString fromEnv = QProcessEnvironment::systemEnvironment().value(
        QStringLiteral("QTG_SIGNING_KEY"));
    if (!fromEnv.isEmpty())
        return seedFromText(fromEnv.toLatin1(), warning);

#ifdef QTG_SIGNING_SEED
    if (warning) {
        *warning = QStringLiteral("正在使用内置演示私钥（公开值），签出的授权不具备防伪能力；"
                                  "正式签发请先 keygen 再用 --key 传入");
    }
    return QByteArray::fromBase64(QByteArray(QTG_SIGNING_SEED));
#else
    if (warning) {
        *warning = QStringLiteral("没有可用的私钥：请用 --key <file> 或环境变量 QTG_SIGNING_KEY 指定");
    }
    return QByteArray();
#endif
}

int doKeygen(const QHash<QString, QString> &opts)
{
    const QByteArray seed = Ed25519::randomSeed();
    QByteArray     pub;
    if (!Ed25519::derivePublicKey(seed, &pub)) {
        err() << "ERROR=cannot derive public key\n";
        err().flush();
        return 3;
    }

    const QString outPath = opts.value(QStringLiteral("out")).trimmed();
    if (!outPath.isEmpty()) {
        QFile key(outPath);
        if (!key.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            err() << "ERROR=cannot write " << outPath << ": " << key.errorString() << "\n";
            err().flush();
            return 3;
        }
        key.write(seed.toBase64() + '\n');
        key.close();
        out() << "FILE=" << QDir::toNativeSeparators(QFileInfo(outPath).absoluteFilePath()) << "\n";
    }

    out() << "SEED=" << QString::fromLatin1(seed.toBase64()) << "\n"
          << "PUBLIC=" << QString::fromLatin1(pub.toBase64()) << "\n"
          << "EMBEDDED="
          << (LicenseManager::signingPublicKey().isEmpty()
                  ? QStringLiteral("NO")
                  : (LicenseManager::signingPublicKey() == pub ? QStringLiteral("MATCH")
                                                              : QStringLiteral("DIFFER")))
          << "\n"
          << "WARN=keep the seed off this machine's shared folders; put PUBLIC into "
             "-DQTG_LICENSE_PUBKEY when building clients\n";
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

    QString keyWarning;
    const QByteArray seed = signingSeed(opts, &keyWarning);
    if (seed.isEmpty()) {
        err() << "ERROR=" << keyWarning << "\n";
        err().flush();
        return 2;
    }
    if (!keyWarning.isEmpty())
        out() << "WARN=" << keyWarning << "\n";

    QString sig;
    QString error;
    if (!LicenseManager::writeLicenseFile(outPath, machine, licensee, edition, issued, expire,
                                          &sig, &error, seed)) {
        err() << "ERROR=" << error << "\n";
        err().flush();
        return 3;
    }

    out() << "ISSUED=" << QFileInfo(outPath).absoluteFilePath() << "\n"
          << "MACHINE=" << LicenseManager::normalizeMachineCode(machine) << "\n"
          << "LICENSEE=" << licensee << "\n"
          << "EDITION=" << edition << "\n"
          << "EXPIRES=" << expire.toString(Qt::ISODate) << "\n"
          << "ALG=ed25519\n"
          << "SIG=" << sig << "\n";

    // 这把私钥派生出的公钥必须与本构建内置的一致，否则客户装上去只会看到“签名无效”
    QByteArray derived;
    Ed25519::derivePublicKey(seed, &derived);
    const QByteArray embedded = LicenseManager::signingPublicKey();
    if (!embedded.isEmpty() && derived != embedded) {
        out() << "WARN=signing key does not match the public key embedded in this build; "
                 "clients built from here will reject this license\n";
    }
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

    QString keyWarning;
    const QByteArray seed = signingSeed(opts, &keyWarning);
    if (seed.isEmpty()) {
        err() << "ERROR=" << keyWarning << "\n";
        err().flush();
        return 2;
    }
    if (!keyWarning.isEmpty())
        out() << "WARN=" << keyWarning << "\n";

    QStringList issueErrors;
    const QVector<BatchResult> results =
        LicenseBatch::issueAll(orders, outDir, dryRun, &issueErrors, seed);
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
    int legacy = 0;   // 仍用 HMAC 签的老授权，提醒逐张换成 Ed25519

    QStringList csvLines;
    csvLines << QStringLiteral("machine,licensee,edition,issued,expire,days_left,state,alg,file");

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
        // 算法与状态无关：过期的 HMAC 授权续费时同样要换成 Ed25519，
        // 只在 Valid 分支里计数会漏掉那些已经到期、正在谈续费的老客户
        if (info.alg == QLatin1String("hmac"))
            ++legacy;
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
              << " ALG=" << info.alg
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
                        csvCell(info.alg),
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
          << " BROKEN=" << broken << " LEGACY=" << legacy
          << " WINDOW=" << window << "\n";
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
          << "ALG=" << info.alg << "\n"
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
    if (command == QLatin1String("keygen"))
        return doKeygen(parseOptions(args, nullptr));
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
