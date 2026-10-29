#include "LicenseBatch.h"

#include "LicenseManager.h"

#include <QDir>
#include <QFileInfo>
#include <QSet>

namespace {

/** 一行 CSV → 字段，支持 "含逗号的值" 与 "" 转义 */
QStringList splitCsvLine(const QString &line)
{
    QStringList fields;
    QString     current;
    bool        inQuotes = false;

    for (int i = 0; i < line.size(); ++i) {
        const QChar ch = line.at(i);
        if (inQuotes) {
            if (ch == QLatin1Char('"')) {
                if (i + 1 < line.size() && line.at(i + 1) == QLatin1Char('"')) {
                    current.append(QLatin1Char('"'));
                    ++i;
                } else {
                    inQuotes = false;
                }
            } else {
                current.append(ch);
            }
        } else if (ch == QLatin1Char('"')) {
            inQuotes = true;
        } else if (ch == QLatin1Char(',')) {
            fields.append(current.trimmed());
            current.clear();
        } else {
            current.append(ch);
        }
    }
    fields.append(current.trimmed());
    return fields;
}

QString csvField(const QString &value)
{
    if (value.contains(QLatin1Char(',')) || value.contains(QLatin1Char('"'))
        || value.contains(QLatin1Char('\n'))) {
        QString escaped = value;
        escaped.replace(QStringLiteral("\""), QStringLiteral("\"\""));
        return QLatin1Char('"') + escaped + QLatin1Char('"');
    }
    return value;
}

/** 从字符串两端去掉 '-' 与 '.'（文件名开头/结尾这两类字符在 Windows 上不合法） */
void trimStemEdges(QString *stem)
{
    while (!stem->isEmpty() && (stem->startsWith(QLatin1Char('-')) || stem->startsWith(QLatin1Char('.'))))
        stem->remove(0, 1);
    while (!stem->isEmpty() && (stem->endsWith(QLatin1Char('-')) || stem->endsWith(QLatin1Char('.'))))
        stem->chop(1);
}

const char kManifestHeader[] = "machine,licensee,edition,issued,expire,file,status,alg,sig8,error";

/** 交付单的数据行（不含表头与 BOM）：写新单与并旧单共用同一段格式化 */
QStringList manifestDataLines(const QVector<BatchResult> &results)
{
    QStringList lines;
    for (const BatchResult &result : results) {
        const BatchOrder &order = result.order;
        lines << QStringList{
                     LicenseManager::normalizeMachineCode(order.machine),
                     csvField(order.licensee),
                     csvField(order.edition),
                     order.issued.isValid() ? order.issued.toString(Qt::ISODate) : QString(),
                     order.expire.isValid() ? order.expire.toString(Qt::ISODate)
                                            : QStringLiteral("perpetual"),
                     csvField(QFileInfo(result.file).fileName()),
                     result.ok() ? QStringLiteral("OK") : QStringLiteral("FAIL"),
                     result.alg,
                     result.sig.left(8),
                     csvField(result.error),
                 }
                     .join(QLatin1Char(','));
    }
    return lines;
}

QByteArray withBom(const QStringList &lines)
{
    // 带 BOM：Excel 双击打开时才会按 UTF-8 解码，否则中文公司名全变乱码
    return QByteArray("\xEF\xBB\xBF") + lines.join(QLatin1Char('\n')).toUtf8() + '\n';
}

/**
 * 把本次结果并进已有交付单。
 *
 * 交付单是账本，不是本次运行的打印稿。客户常常先签三家、过两天再补两家，
 * 直接 Truncate 会把上一批的行冲掉：目录里 5 个 .qglic，账上只剩 2 行，
 * 续费时谁也说不清另外 3 家是哪天签的、签给谁。
 * 同一文件名以本次为准（重签会覆盖同名文件，账也得跟着改）。
 */
QByteArray mergeManifest(const QString &path, const QVector<BatchResult> &results)
{
    const QStringList fresh = manifestDataLines(results);

    QStringList freshFiles;
    for (const BatchResult &result : results)
        freshFiles << QFileInfo(result.file).fileName().toLower();

    QStringList kept;
    QFile old(path);
    if (old.open(QIODevice::ReadOnly)) {
        QByteArray data = old.readAll();
        old.close();
        if (data.startsWith("\xEF\xBB\xBF"))
            data.remove(0, 3);

        const QStringList lines = QString::fromUtf8(data).split(QLatin1Char('\n'));
        for (const QString &raw : lines) {
            QString line = raw;
            if (line.endsWith(QLatin1Char('\r')))
                line.chop(1);
            if (line.trimmed().isEmpty())
                continue;
            if (line.startsWith(QLatin1String("machine,licensee")))   // 旧表头
                continue;
            const QStringList fields = splitCsvLine(line);
            if (fields.size() >= 6 && freshFiles.contains(fields.at(5).toLower()))
                continue;   // 本次重写了同一份文件
            kept << line;
        }
    }

    kept << fresh;
    QStringList out;
    out << QString::fromLatin1(kManifestHeader) << kept;
    return withBom(out);
}

} // namespace

QVector<BatchOrder> LicenseBatch::parseLedger(const QByteArray &content, QStringList *errors)
{
    QVector<BatchOrder> orders;

    QByteArray data = content;
    if (data.startsWith("\xEF\xBB\xBF"))   // Excel 存出来的 UTF-8 CSV 自带 BOM
        data.remove(0, 3);

    const QString     text  = QString::fromUtf8(data);
    const QStringList lines = text.split(QLatin1Char('\n'));

    const QString today = QDate::currentDate().toString(Qt::ISODate);

    bool headerChecked = false;   // 表头只可能在第一条数据行，前面允许有注释

    for (int i = 0; i < lines.size(); ++i) {
        const int lineNo = i + 1;

        QString line = lines.at(i);
        if (line.endsWith(QLatin1Char('\r')))
            line.chop(1);

        const QString trimmed = line.trimmed();
        if (trimmed.isEmpty() || trimmed.startsWith(QLatin1Char('#')))
            continue;

        const QStringList fields = splitCsvLine(line);

        // 表头可有可无：首个字段叫 machine 或“机器码”就跳过
        if (!headerChecked) {
            headerChecked = true;
            const QString head = fields.value(0).trimmed();
            if (head.compare(QStringLiteral("machine"), Qt::CaseInsensitive) == 0
                || head == QStringLiteral("机器码")) {
                continue;
            }
        }

        auto reject = [&](const QString &reason) {
            if (errors)
                errors->append(QStringLiteral("第 %1 行：%2").arg(lineNo).arg(reason));
        };

        BatchOrder order;
        order.line     = lineNo;
        order.machine  = fields.value(0).trimmed();
        order.licensee = fields.value(1).trimmed();
        order.edition  = fields.value(2).trimmed();
        order.issued   = QDate::currentDate();

        if (order.edition.isEmpty())
            order.edition = QStringLiteral("pro");

        if (order.machine.isEmpty()) {
            reject(QStringLiteral("机器码为空"));
            continue;
        }
        if (!LicenseManager::machineCodeLooksValid(order.machine)) {
            reject(QStringLiteral("机器码格式不对（应为 8 组 4 位十六进制，连字符可省）：%1")
                       .arg(order.machine));
            continue;
        }
        if (order.licensee.isEmpty()) {
            reject(QStringLiteral("授权对象为空"));
            continue;
        }

        const QString daysText = fields.value(3).trimmed();
        if (!daysText.isEmpty()) {
            bool      ok   = false;
            const int days = daysText.toInt(&ok);
            if (!ok || days <= 0) {
                reject(QStringLiteral("days 不是正整数：%1").arg(daysText));
                continue;
            }
            order.days = days;
        }

        const QString expireText = fields.value(4).trimmed();
        if (!expireText.isEmpty()) {
            order.expire = QDate::fromString(expireText, Qt::ISODate);
            if (!order.expire.isValid()) {
                reject(QStringLiteral("expire 不是 yyyy-MM-dd：%1").arg(expireText));
                continue;
            }
        } else if (order.days > 0) {
            order.expire = order.issued.addDays(order.days);
        }

        if (!order.expire.isValid()) {
            reject(QStringLiteral("days 与 expire 至少要填一个"));
            continue;
        }
        if (order.expire < order.issued) {
            reject(QStringLiteral("到期日 %1 早于今天（%2）").arg(expireText, today));
            continue;
        }

        orders.append(order);
    }

    // 一行都没有时也要说清楚：不是“签完了”，是台账没读进来
    if (orders.isEmpty() && errors && errors->isEmpty())
        errors->append(QStringLiteral("台账里没有可用的数据行（表头%s）")
                           .arg(QStringLiteral("machine,licensee,edition,days,expire")));

    return orders;
}

QVector<BatchResult> LicenseBatch::issueAll(const QVector<BatchOrder> &orders,
                                            const QString &outDir, bool dryRun,
                                            QStringList *errors,
                                            const QByteArray &signingSeed)
{
    QVector<BatchResult> results;
    results.reserve(orders.size());

    if (!dryRun && !QDir().mkpath(outDir)) {
        if (errors)
            errors->append(QStringLiteral("无法创建输出目录：%1").arg(outDir));
        return results;
    }

    QSet<QString> used;   // 同一批里两家重名（同名同到期日）时加序号，不许悄悄覆盖

    // 上一批留在目录里的同名文件也算“用过了”：客户在同一目录里补签一单，
    // 不能把上次那份顶掉——文件换了，交付单上那一行的 sig8 就对不上了。
    // （dry-run 不建目录，entryList 拿不到东西，正好什么也不影响）
    for (const QString &existing : QDir(outDir).entryList({QStringLiteral("*.qglic")}, QDir::Files))
        used.insert(existing.toLower());

    for (const BatchOrder &order : orders) {
        BatchResult result;
        result.order = order;
        // 算法跟着这一次调用走，不跟单个订单走；dry-run 也要填，交付单上的计划得能对得上
        result.alg = signingSeed.isEmpty() ? QStringLiteral("hmac") : QStringLiteral("ed25519");

        QString stem = fileNameFor(order);
        if (stem.endsWith(QStringLiteral(".qglic"), Qt::CaseInsensitive))
            stem.chop(6);

        QString candidate = stem + QStringLiteral(".qglic");
        int     suffix    = 1;
        while (used.contains(candidate.toLower())) {
            ++suffix;
            candidate = QStringLiteral("%1-%2.qglic").arg(stem).arg(suffix);
        }
        used.insert(candidate.toLower());

        result.file = QDir(outDir).filePath(candidate);

        if (!dryRun) {
            QString error;
            if (!LicenseManager::writeLicenseFile(result.file, order.machine, order.licensee,
                                                  order.edition, order.issued, order.expire,
                                                  &result.sig, &error, signingSeed)) {
                result.error = error;
            }
        }
        results.append(result);
    }

    // 交付单与授权文件一起落地：只发文件不记账，三批之后就说不清谁买到哪天了
    if (!dryRun) {
        int okCount = 0;
        for (const BatchResult &result : results)
            okCount += result.ok() ? 1 : 0;

        if (okCount > 0) {
            // 先并再开：写模式是 Truncate，先打开会把要保留的旧行清掉
            const QByteArray merged = mergeManifest(manifestPath(outDir), results);
            QFile manifest(manifestPath(outDir));
            if (manifest.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                manifest.write(merged);
                manifest.close();
            } else if (errors) {
                errors->append(QStringLiteral("交付单写入失败：%1").arg(manifest.errorString()));
            }
        }
    }

    return results;
}

QString LicenseBatch::manifestPath(const QString &outDir)
{
    return QDir(outDir).filePath(QStringLiteral("manifest.csv"));
}

QByteArray LicenseBatch::manifestCsv(const QVector<BatchResult> &results)
{
    QStringList lines;
    lines << QString::fromLatin1(kManifestHeader);
    lines << manifestDataLines(results);
    return withBom(lines);
}

QString LicenseBatch::safeFileStem(const QString &licensee, const QString &machine)
{
    const QString illegal = QStringLiteral("\\/:*?\"<>|,;");

    QString stem;
    for (const QChar ch : licensee.trimmed()) {
        if (ch.category() == QChar::Other_Control)
            continue;
        if (ch.isSpace() || illegal.contains(ch))
            stem.append(QLatin1Char('-'));
        else
            stem.append(ch);
    }

    while (stem.contains(QStringLiteral("--")))
        stem.replace(QStringLiteral("--"), QStringLiteral("-"));
    trimStemEdges(&stem);

    // 太长会在某些解压工具里被截断，留 40 个字符足够认出是谁
    if (stem.size() > 40) {
        stem.truncate(40);
        trimStemEdges(&stem);
    }

    if (stem.isEmpty()) {
        const QString digits = LicenseManager::normalizeMachineCode(machine).remove(QLatin1Char('-'));
        stem = digits.left(8);
    }
    return stem;
}

QString LicenseBatch::fileNameFor(const BatchOrder &order)
{
    const QString expire = order.expire.isValid() ? order.expire.toString(Qt::ISODate)
                                                  : QStringLiteral("perpetual");
    return QStringLiteral("qglic-%1-%2.qglic")
        .arg(safeFileStem(order.licensee, order.machine), expire);
}
