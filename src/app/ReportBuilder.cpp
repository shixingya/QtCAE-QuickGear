#include "ReportBuilder.h"

#include <QAbstractTextDocumentLayout>
#include <QDateTime>
#include <QFileInfo>
#include <QFont>
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
    body += row(QStringLiteral("云图量程 (MPa)"), QString::number(r.stressCeiling, 'f', 0));
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
    QPdfWriter writer(path);
    writer.setPageSize(QPageSize(QPageSize::A4));
    writer.setPageMargins(QMarginsF(14.0, 13.0, 14.0, 13.0), QPageLayout::Millimeter);
    writer.setResolution(150);
    writer.setTitle(QStringLiteral("QtCAE-QuickGear 校核结论"));
    writer.setCreator(QStringLiteral("QtCAE-QuickGear %1").arg(ctx.appVersion));

    QTextDocument doc;
    doc.setDefaultFont(QFont(QStringLiteral("Microsoft YaHei"), 8));
    doc.addResource(QTextDocument::ImageResource, QUrl(QStringLiteral("quickgear:stress.png")),
                    ctx.stressImage);
    doc.setHtml(buildReportHtml(ctx));

    // 自己翻页：QTextDocument::print() 会按满页重新排版再缩放，
    // 结果比按内边距排出来的高出一大截，单页结论会被挤成两页。
    const QRect pageRect = writer.pageLayout().paintRectPixels(writer.resolution());
    const QSizeF body(pageRect.width(), pageRect.height());

    QPainter painter(&writer);
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

    const QFileInfo info(path);
    if (!info.exists() || info.size() == 0) {
        *error = QStringLiteral("PDF 未写出，请检查目录写权限：%1").arg(info.absolutePath());
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

    QFont font(QStringLiteral("Microsoft YaHei"));
    font.setPointSizeF(qMax(12.0, out.width() / 24.0));
    font.setBold(true);
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
