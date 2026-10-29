#include "ReportBuilder.h"

#include "StressScale.h"

#include <QAbstractTextDocumentLayout>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QLocale>
#include <QPageLayout>
#include <QPageSize>
#include <QPainter>
#include <QPdfWriter>
#include <QStringList>
#include <QTextDocument>
#include <QTextLayout>
#include <QUrl>

namespace {

const char kPassLine[]  = "1.25";
const char kInk        [] = "#1E3A5F";
const char kMuted      [] = "#475569";
const char kRule       [] = "#D7DEE7";

QString esc(const QString &text)
{
    return text.toHtmlEscaped();
}

QString row(const QString &label, const QString &value)
{
    return QStringLiteral("<tr>"
                          "<td width='38%' bgcolor='#F4F7FA' style='color:%1'>%2</td>"
                          "<td style='color:#0F172A'><b>%3</b></td>"
                          "</tr>")
        .arg(QString::fromLatin1(kMuted), esc(label), esc(value));
}

QString maskedMachine(const QString &machine)
{
    if (machine.size() <= 10)
        return machine;
    return machine.left(9) + QStringLiteral("…") + machine.right(4);
}

} // namespace

QFont reportFont(double pointSize, bool bold)
{
    // 候选按“汉字覆盖 + Windows 上常见”的顺序排；第一个真能用上的就拿它
    static const QStringList candidates{
        QStringLiteral("Microsoft YaHei"),
        QStringLiteral("Microsoft YaHei UI"),
        QStringLiteral("SimHei"),
        QStringLiteral("SimSun"),
        QStringLiteral("Noto Sans CJK SC"),
        QStringLiteral("Source Han Sans SC"),
    };
    const QStringList installed = QFontDatabase().families();

    QString chosen;
    for (const QString &family : candidates) {
        if (!installed.contains(family, Qt::CaseInsensitive))
            continue;
        QFont probe(family);
        // 装了字体也可能缺字形（裁剪过的系统包），拿一个报告里必出现的字试一下
        if (QFontMetrics(probe).inFont(QChar(0x9F7F))) {   // 齿
            chosen = family;
            break;
        }
    }

    QFont font;
    if (!chosen.isEmpty())
        font.setFamily(chosen);
    // 一个候选都没有就交给系统默认字体：至少还能显示，比空白强
    font.setPointSizeF(pointSize);
    font.setBold(bold);
    return font;
}

QString conclusionText(const SolveResult &result, bool *passed)
{
    const QString sf = QString::number(result.safetyFactor, 'f', 2);
    if (passed)
        *passed = result.safetyFactor >= 1.25;

    if (result.safetyFactor >= 1.25)
        return QStringLiteral("通过：安全系数 n = %1 ≥ %2，按当前参数可继续细化（示意后端结论）。")
            .arg(sf, QString::fromLatin1(kPassLine));
    if (result.safetyFactor >= 1.0)
        return QStringLiteral("边界：安全系数 n = %1，落在 1.00~%2 之间，建议加大模数或齿宽后复算。")
            .arg(sf, QString::fromLatin1(kPassLine));
    return QStringLiteral("不满足：安全系数 n = %1 < 1.00，应力已超出材料许用值。").arg(sf);
}

QString buildReportHtml(const ReportContext &ctx, const QString &imageSrc)
{
    const GearParams &p  = ctx.params;
    const SolveResult &r = ctx.result;

    bool passed = false;
    const QString verdict = conclusionText(r, &passed);

    // 云图量程写死 320（与网页端同一套口径），峰值却能到 640：超了之后图上最热的
    // 几个齿全是一个颜色，不写清楚客户会把“最红”读成“最高就 320”。
    // 量程为 0（手改过的工程文件）时回落到默认值，不在报告里印一个“量程 0”。
    const double ceiling = r.stressCeiling > 0.0 ? r.stressCeiling : 320.0;
    const QString saturationNote = stressscale::saturationNote(r.peakVonMises, ceiling);
    const QString verdictColor = passed ? QStringLiteral("#0B7A5B")
                                        : (r.safetyFactor >= 1.0 ? QStringLiteral("#A2652A")
                                                                 : QStringLiteral("#B7352F"));

    // 签名算法一并写进页脚：客户拿着一份 PDF 就能看出它是公钥签的还是旧共享密钥签的，
    // 不用拆授权文件也不用来问我们
    const QString licenseLine = ctx.licensed
        ? QStringLiteral("已授权（%1）· 签名 %2")
              .arg(esc(ctx.licensee),
                   ctx.licenseAlg.isEmpty() ? QStringLiteral("未知") : esc(ctx.licenseAlg))
        : QStringLiteral("未授权 · 试用版导出");

    QString body;
    body += QStringLiteral(
                "<h2 style='color:%1;margin-bottom:2px'>齿轮 CAE 校核结论</h2>"
                "<p style='color:%2;margin-top:0'>QtCAE-QuickGear %3 · 生成于 %4</p>"
                "<hr style='color:%5'/>")
                .arg(QString::fromLatin1(kInk), QString::fromLatin1(kMuted),
                     esc(ctx.appVersion),
                     esc(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm"))),
                     QString::fromLatin1(kRule));

    body += QStringLiteral("<p style='color:%1;font-size:13px'><b>%2</b></p>")
                .arg(verdictColor, esc(verdict));

    if (ctx.demo) {
        body += QStringLiteral("<p style='color:#B4692F'>注意：本报告由内置示意后端生成，"
                               "数值基于经验公式，仅用于流程沟通，不可作为设计或验收依据。</p>");
    }

    body += QStringLiteral("<h3 style='color:%1;margin-bottom:4px'>输入参数</h3>")
                .arg(QString::fromLatin1(kInk));
    body += QStringLiteral("<table width='100%' cellspacing='0' cellpadding='5' border='1' "
                           "style='border-color:%1'>").arg(QString::fromLatin1(kRule));
    body += row(QStringLiteral("主动轮齿数 z₁"), QString::number(p.teeth));
    body += row(QStringLiteral("从动轮齿数 z₂"), QString::number(p.pinionTeeth));
    body += row(QStringLiteral("模数 m (mm)"), QString::number(p.module, 'f', 2));
    body += row(QStringLiteral("输入扭矩 T (N·m)"), QString::number(p.torque, 'f', 1));
    body += row(QStringLiteral("齿宽 b (mm)"), QString::number(p.faceWidth, 'f', 1));
    body += row(QStringLiteral("材料"), p.material);
    // 把许用应力一并写出来：客户拿到 PDF 能用 n = 许用 / σv 自己核一遍，
    // 不用回来问“你这个安全系数是怎么算的”
    body += row(QStringLiteral("材料许用应力 (MPa)"),
                QString::number(GearParamLimits::allowableStressMPa(p.material), 'f', 0));
    body += row(QStringLiteral("分度圆 / 齿顶圆 (mm)"),
                QStringLiteral("%1 / %2")
                    .arg(QString::number(p.pitchDiameter(), 'f', 1),
                         QString::number(p.tipDiameter(), 'f', 1)));
    if (!ctx.meshSummary.isEmpty())
        body += row(QStringLiteral("导入几何"), ctx.meshSummary);
    body += QStringLiteral("</table>");

    body += QStringLiteral("<h3 style='color:%1;margin-bottom:4px'>计算结果</h3>")
                .arg(QString::fromLatin1(kInk));
    body += QStringLiteral("<table width='100%' cellspacing='0' cellpadding='5' border='1' "
                           "style='border-color:%1'>").arg(QString::fromLatin1(kRule));
    body += row(QStringLiteral("峰值等效应力 σv (MPa)"), QString::number(r.peakVonMises, 'f', 1));
    body += row(QStringLiteral("最大位移 (μm)"), QString::number(r.maxDeflection, 'f', 1));
    body += row(QStringLiteral("安全系数 n"), QString::number(r.safetyFactor, 'f', 2));
    body += row(QStringLiteral("网格单元 / 节点"),
                QStringLiteral("%1 / %2")
                    .arg(QLocale().toString(r.elements), QLocale().toString(r.nodes)));
    body += row(QStringLiteral("求解耗时 (s)"), QString::number(r.solveSeconds, 'f', 2));
    body += row(QStringLiteral("云图量程 (MPa)"),
                saturationNote.isEmpty()
                    ? QString::number(ceiling, 'f', 0)
                    : QStringLiteral("%1（%2）")
                          .arg(QString::number(ceiling, 'f', 0), saturationNote));
    body += row(QStringLiteral("求解后端"), r.backend);
    body += QStringLiteral("</table>");

    if (!ctx.stressImage.isNull()) {
        body += QStringLiteral("<h3 style='color:%1;margin-bottom:4px'>等效应力云图</h3>")
                    .arg(QString::fromLatin1(kInk));
        // 680 是“看着够大”与“一定排进一页”的交界：到 760 就会溢出成两页
        body += QStringLiteral("<p><img src='%1' width='%2'/></p>")
                    .arg(imageSrc)
                    .arg(qMin(ctx.stressImage.width(), 680));
    }

    if (!ctx.note.isEmpty()) {
        body += QStringLiteral("<h3 style='color:%1;margin-bottom:4px'>工况备注</h3>")
                    .arg(QString::fromLatin1(kInk));
        body += QStringLiteral("<p>%1</p>").arg(esc(ctx.note));
    }

    body += QStringLiteral(
                "<hr style='color:%1'/>"
                "<p style='color:%2;font-size:8px'>"
                "机器码 %3 · 授权状态 %4<br/>"
                "免责声明：本报告由 QtCAE-QuickGear 生成。示意图与数值的比例、边界条件按默认工况设定，"
                "与实际零件存在差异；正式设计须以经标定的求解器与实验数据为准。"
                "</p>")
                .arg(QString::fromLatin1(kRule), QString::fromLatin1(kMuted),
                     esc(maskedMachine(ctx.machineCode)), licenseLine);

    return QStringLiteral("<html><body style='color:#0F172A'>%1</body></html>").arg(body);
}

bool writeReportPdf(const QString &path, const ReportContext &ctx, QString *error)
{
    const QFileInfo target(path);
    // 先写同目录的临时文件，成功后再换名。直接写目标文件有两个坑：
    //   1. 覆盖上一份报告时若写入失败（文件开在阅读器里、盘满、目录被策略锁），
    //      旧文件大小仍然 > 0，下面的存在性检查会把失败当成功报出去；
    //   2. 中途失败会把上一份能用的报告写坏一半。
    const QString tempPath = target.absolutePath() + QLatin1Char('/') + QLatin1Char('.')
                             + target.fileName() + QStringLiteral(".part");

    QPdfWriter writer(tempPath);
    writer.setPageSize(QPageSize(QPageSize::A4));
    writer.setPageMargins(QMarginsF(14.0, 13.0, 14.0, 13.0), QPageLayout::Millimeter);
    writer.setResolution(150);
    writer.setTitle(QStringLiteral("QtCAE-QuickGear 校核结论"));
    writer.setCreator(QStringLiteral("QtCAE-QuickGear %1").arg(ctx.appVersion));

    QTextDocument doc;
    doc.setDefaultFont(reportFont(8.0));
    doc.addResource(QTextDocument::ImageResource, QUrl(QStringLiteral("quickgear:stress.png")),
                    ctx.stressImage);
    doc.setHtml(buildReportHtml(ctx));

    // 自己翻页：QTextDocument::print() 会按满页重新排版再缩放，
    // 结果比按内边距排出来的高出一大截，单页结论会被挤成两页。
    const QRect pageRect = writer.pageLayout().paintRectPixels(writer.resolution());
    const QSizeF body(pageRect.width(), pageRect.height());

    QPainter painter(&writer);
    if (!painter.isActive()) {
        // 连临时文件都开不出来：目录不存在/不可写，或磁盘已满
        QFile::remove(tempPath);
        *error = QStringLiteral("无法写入 PDF，请检查目录写权限与剩余空间：%1")
                     .arg(QDir::toNativeSeparators(target.absolutePath()));
        return false;
    }
    doc.documentLayout()->setPaintDevice(painter.device());
    doc.setPageSize(body);

    const int pages = doc.pageCount();
    for (int i = 0; i < pages; ++i) {
        if (i > 0)
            writer.newPage();
        painter.save();
        painter.setClipRect(pageRect);
        painter.translate(pageRect.topLeft());
        painter.translate(0, -i * body.height());
        doc.drawContents(&painter, QRectF(0, i * body.height(), body.width(), body.height()));
        painter.restore();
    }
    painter.end();

    const QFileInfo tempInfo(tempPath);
    if (!tempInfo.exists() || tempInfo.size() == 0) {
        QFile::remove(tempPath);
        *error = QStringLiteral("PDF 未写出，请检查目录写权限：%1")
                     .arg(QDir::toNativeSeparators(target.absolutePath()));
        return false;
    }

    // Windows 的 rename 不覆盖已有文件，先删旧的；删不掉就是有人正开着它
    if (QFile::exists(path) && !QFile::remove(path)) {
        QFile::remove(tempPath);
        *error = QStringLiteral("无法覆盖 %1：文件可能正被 PDF 阅读器占用，请关闭后重试")
                     .arg(QDir::toNativeSeparators(path));
        return false;
    }
    if (!QFile::rename(tempPath, path)) {
        QFile::remove(tempPath);
        *error = QStringLiteral("PDF 换名失败：%1").arg(QDir::toNativeSeparators(path));
        return false;
    }
    return true;
}

bool writeImagePng(const QString &path, const QImage &image, QString *error)
{
    if (image.isNull()) {
        *error = QStringLiteral("没有可导出的图像");
        return false;
    }
    if (!image.save(path, "PNG")) {
        *error = QStringLiteral("图片写入失败：%1").arg(path);
        return false;
    }
    return true;
}

QImage watermarkTrial(const QImage &image, const QString &text)
{
    if (image.isNull())
        return image;

    QImage out = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    QPainter painter(&out);
    painter.setRenderHint(QPainter::Antialiasing, true);

    QFont font = reportFont(qMax(12.0, out.width() / 24.0), true);
    painter.setFont(font);
    // 云图背景是深色，水印用浅色低透明，既不遮数据又能一眼看出来
    painter.setPen(QColor(0xE2, 0xE8, 0xF0, 70));
    painter.translate(out.width() / 2.0, out.height() / 2.0);
    painter.rotate(-24.0);

    const QStringList lines{text, text};
    const double step = font.pointSizeF() * 4.2;
    for (int i = 0; i < lines.size(); ++i) {
        const double offset = (i - (lines.size() - 1) / 2.0) * step;
        painter.drawText(QRectF(-out.width() * 0.5, offset - step * 0.5,
                                out.width(), step),
                         Qt::AlignCenter, lines.at(i));
    }
    painter.end();
    return out;
}
