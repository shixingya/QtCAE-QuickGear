#include "app/ReportBuilder.h"
#include "app/StressView.h"
#include "core/LicenseManager.h"
#include "core/ProjectFile.h"
#include "core/SolverFactory.h"
#include "core/StlMesh.h"

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QImage>
#include <QPainter>
#include <QTextDocument>
#include <QTextStream>
#include <QUrl>
#include <QVector>

#include <cmath>
#include <cstring>

/**
 * qgselftest：不依赖鼠标的冒烟自检。
 *
 * 覆盖流程里最容易悄悄坏掉、又最难靠肉眼确认的四段：
 *   STL 解析（ASCII + 二进制） / 求解 / 结论页导出 / 工程文件往返 / 授权验签
 *
 * 用法：qgselftest.exe [--out <目录>]   退出码 0 = 全过
 */

namespace {

int g_failures = 0;
QTextStream g_out(stdout);

void check(bool condition, const QString &name, const QString &detail)
{
    g_out << (condition ? "PASS " : "FAIL ") << name << " :: " << detail << Qt::endl;
    if (!condition)
        ++g_failures;
}

struct Tri
{
    float v[3][3];
};

QVector<Tri> makeGearSolid(int teeth, double tipRadius, double rootRadius, double thickness)
{
    const int    segments = teeth * 2;
    const double pi       = 3.14159265358979323846;
    const double halfZ    = thickness / 2.0;

    QVector<double> angles;
    QVector<double> radii;
    for (int i = 0; i < segments; ++i) {
        angles.append(2.0 * pi * i / segments);
        radii.append((i % 2 == 0) ? tipRadius : rootRadius);
    }

    QVector<Tri> tris;

    // 上下底盖：以圆心为中心的扇形三角形
    for (int i = 0; i < segments; ++i) {
        const int    j = (i + 1) % segments;
        const double x0 = radii[i] * std::cos(angles[i]);
        const double y0 = radii[i] * std::sin(angles[i]);
        const double x1 = radii[j] * std::cos(angles[j]);
        const double y1 = radii[j] * std::sin(angles[j]);

        Tri bottom;
        std::memset(&bottom, 0, sizeof(bottom));
        bottom.v[0][0] = 0.0f;
        bottom.v[0][1] = 0.0f;
        bottom.v[0][2] = float(-halfZ);
        bottom.v[1][0] = float(x1);
        bottom.v[1][1] = float(y1);
        bottom.v[1][2] = float(-halfZ);
        bottom.v[2][0] = float(x0);
        bottom.v[2][1] = float(y0);
        bottom.v[2][2] = float(-halfZ);
        tris.append(bottom);

        Tri top;
        std::memset(&top, 0, sizeof(top));
        top.v[0][0] = 0.0f;
        top.v[0][1] = 0.0f;
        top.v[0][2] = float(halfZ);
        top.v[1][0] = float(x0);
        top.v[1][1] = float(y0);
        top.v[1][2] = float(halfZ);
        top.v[2][0] = float(x1);
        top.v[2][1] = float(y1);
        top.v[2][2] = float(halfZ);
        tris.append(top);
    }

    // 侧壁
    for (int i = 0; i < segments; ++i) {
        const int    j  = (i + 1) % segments;
        const double x0 = radii[i] * std::cos(angles[i]);
        const double y0 = radii[i] * std::sin(angles[i]);
        const double x1 = radii[j] * std::cos(angles[j]);
        const double y1 = radii[j] * std::sin(angles[j]);

        const float quad[4][3] = {
            {float(x0), float(y0), float(-halfZ)},
            {float(x1), float(y1), float(-halfZ)},
            {float(x1), float(y1), float(halfZ)},
            {float(x0), float(y0), float(halfZ)},
        };
        const int order[2][3] = {{0, 1, 2}, {0, 2, 3}};
        for (const auto &idx : order) {
            Tri t;
            std::memset(&t, 0, sizeof(t));
            for (int k = 0; k < 3; ++k)
                std::memcpy(t.v[k], quad[idx[k]], sizeof(t.v[k]));
            tris.append(t);
        }
    }

    return tris;
}

bool writeAsciiStl(const QString &path, const QVector<Tri> &tris)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        return false;

    QTextStream out(&file);
    out << "solid quickgear_selftest\n";
    for (const Tri &t : tris) {
        out << "  facet normal 0 0 0\n    outer loop\n";
        for (const auto &v : t.v)
            out << "      vertex " << v[0] << ' ' << v[1] << ' ' << v[2] << "\n";
        out << "    endloop\n  endfacet\n";
    }
    out << "endsolid quickgear_selftest\n";
    return true;
}

bool writeBinaryStl(const QString &path, const QVector<Tri> &tris)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;

    QByteArray blob(80, '\0');
    std::memcpy(blob.data(), "qgselftest binary", 17);

    const quint32 count = quint32(tris.size());
    blob.append(reinterpret_cast<const char *>(&count), 4);

    for (const Tri &t : tris) {
        const float zero[3] = {0.0f, 0.0f, 0.0f};
        blob.append(reinterpret_cast<const char *>(zero), 12);
        for (const auto &v : t.v)
            blob.append(reinterpret_cast<const char *>(v), 12);
        const quint16 attr = 0;
        blob.append(reinterpret_cast<const char *>(&attr), 2);
    }

    return file.write(blob) == blob.size();
}

QString argValue(const QStringList &args, const QString &key, const QString &fallback)
{
    const int index = args.indexOf(key);
    if (index >= 0 && index + 1 < args.size())
        return args.at(index + 1);
    return fallback;
}

/**
 * 从 PDF 字节里数页数（读目录对象的 /Count），用于守住"结论页必须一页"。
 */
int pdfPageCount(const QByteArray &bytes)
{
    const int at = bytes.indexOf("/Count ");
    if (at < 0)
        return -1;
    int i = at + 7;
    int value = 0;
    while (i < bytes.size() && bytes.at(i) >= '0' && bytes.at(i) <= '9') {
        value = value * 10 + (bytes.at(i) - '0');
        ++i;
    }
    return value;
}

/**
 * 画面覆盖率：排除右侧色标条后统计"非背景"像素占比。
 * requireColor=true 只算饱和像素，用于确认云图真的上色了。
 * 这是当初真发现过"视口空白"的那类缺陷的回归断言。
 */
double coverage(const QImage &image, bool requireColor)
{
    const int width = image.width() - 100;
    int hits  = 0;
    int total = 0;
    for (int y = 0; y < image.height(); y += 2) {
        for (int x = 0; x < width; x += 2) {
            const QColor c = image.pixelColor(x, y);
            ++total;
            const int hi = qMax(c.red(), qMax(c.green(), c.blue()));
            const int lo = qMin(c.red(), qMin(c.green(), c.blue()));
            if (requireColor) {
                if (hi - lo > 30 && hi > 60)
                    ++hits;
            } else if (hi > 45) {
                ++hits;
            }
        }
    }
    return total > 0 ? double(hits) / double(total) : 0.0;
}

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    // 与客户端保持一致，授权目录才能对齐
    QApplication::setApplicationName(QStringLiteral("QtCAE-QuickGear"));
    QApplication::setApplicationVersion(QStringLiteral("0.2.0"));
    QApplication::setOrganizationName(QStringLiteral("shixingya"));

    const QStringList args = QApplication::arguments();
    const QString outDir = argValue(
        args, QStringLiteral("--out"),
        QDir::temp().filePath(QStringLiteral("qg-selftest-%1")
                                  .arg(QDateTime::currentDateTime().toString(QStringLiteral("HHmmss")))));
    QDir().mkpath(outDir);
    g_out << "output dir: " << QDir::toNativeSeparators(outDir) << Qt::endl;

    // ---- 1. STL：ASCII 与二进制都要读得进，且统计量一致 ----
    const QVector<Tri> gear = makeGearSolid(12, 40.0, 30.0, 20.0);
    const QString asciiPath = QDir(outDir).filePath(QStringLiteral("gear_ascii.stl"));
    const QString binPath   = QDir(outDir).filePath(QStringLiteral("gear_binary.stl"));

    check(writeAsciiStl(asciiPath, gear), QStringLiteral("stl-write-ascii"),
          QStringLiteral("%1 三角形").arg(gear.size()));
    check(writeBinaryStl(binPath, gear), QStringLiteral("stl-write-binary"),
          QStringLiteral("%1 三角形").arg(gear.size()));

    const StlLoadResult ascii = loadStl(asciiPath);
    check(ascii.ok && ascii.format == StlLoadResult::Ascii, QStringLiteral("stl-parse-ascii"),
          ascii.ok ? stlSummary(ascii.mesh) : ascii.error);

    const StlLoadResult binary = loadStl(binPath);
    check(binary.ok && binary.format == StlLoadResult::Binary, QStringLiteral("stl-parse-binary"),
          binary.ok ? stlSummary(binary.mesh) : binary.error);

    if (ascii.ok && binary.ok) {
        const double dv = std::fabs(ascii.mesh.volume - binary.mesh.volume);
        check(ascii.mesh.count() == binary.mesh.count() && dv < 1.0,
              QStringLiteral("stl-format-consistency"),
              QStringLiteral("体积 %1 vs %2 cm³")
                  .arg(ascii.mesh.volume / 1000.0, 0, 'f', 3)
                  .arg(binary.mesh.volume / 1000.0, 0, 'f', 3));
        check(ascii.mesh.volume > 0.0 && ascii.mesh.area > 0.0,
              QStringLiteral("stl-stats"),
              QStringLiteral("体积 %1 cm³ · 面积 %2 cm²")
                  .arg(ascii.mesh.volume / 1000.0, 0, 'f', 2)
                  .arg(ascii.mesh.area / 100.0, 0, 'f', 2));
    }

    // ---- 2. 求解（内置 JSON 后端）----
    QScopedPointer<ISolverBackend> backend(createSolverBackend());
    check(!backend.isNull(), QStringLiteral("backend-create"),
          backend ? backend->displayName() : QStringLiteral("空指针"));
    if (!backend)
        return 1;

    GearParams params;
    const SolveResult result = backend->solve(params, [](int, const QString &) {});
    check(result.ok && result.peakVonMises > 0.0 && result.toothStress.size() > 0,
          QStringLiteral("solve"),
          QStringLiteral("σv=%1 MPa · n=%2 · %3 单元")
              .arg(result.peakVonMises, 0, 'f', 1)
              .arg(result.safetyFactor, 0, 'f', 2)
              .arg(result.elements));

    // ---- 3. 结论页导出（离屏渲染云图，不弹窗口）----
    StressView view;
    view.setAttribute(Qt::WA_DontShowOnScreen, true);
    view.resize(760, 600);   // 与客户端导出用的尺寸一致
    view.setGear(params.teeth, params.pinionTeeth, params.module);
    view.setDemoMode(true);
    if (ascii.ok)
        view.setMesh(ascii.mesh);
    view.setResult(result);
    view.show();

    const QImage shot = view.grab().toImage();
    check(!shot.isNull() && shot.width() > 100, QStringLiteral("render-stress-view"),
          QStringLiteral("%1×%2 px").arg(shot.width()).arg(shot.height()));
    check(coverage(shot, true) > 0.02, QStringLiteral("render-cloud-map-colored"),
          QStringLiteral("饱和像素占比 %1%")
              .arg(coverage(shot, true) * 100.0, 0, 'f', 1));

    // 未求解阶段应显示导入几何的投影（曾因绘制顺序错误整块空白）
    QImage meshPreview;
    if (ascii.ok) {
        view.clearResult();
        meshPreview = view.grab().toImage();
        const QString meshPng = QDir(outDir).filePath(QStringLiteral("mesh_preview.png"));
        QString meshError;
        check(!meshPreview.isNull() && coverage(meshPreview, false) > 0.05
                  && writeImagePng(meshPng, meshPreview, &meshError),
              QStringLiteral("render-mesh-preview"),
              QStringLiteral("非背景像素占比 %1%")
                  .arg(coverage(meshPreview, false) * 100.0, 0, 'f', 1));
        view.setResult(result);
    }

    // 与客户端一致：未授权导出给云图打水印，PDF 与 PNG 用同一张
    const QImage watermarked = watermarkTrial(shot, QStringLiteral("未授权试用 · 示意数据"));

    ReportContext ctx;
    ctx.params      = params;
    ctx.result      = result;
    ctx.stressImage = watermarked;
    ctx.machineCode = LicenseManager::machineCode();
    ctx.licensed    = false;
    ctx.demo        = backend->isDemo();
    ctx.meshSummary = ascii.ok ? stlSummary(ascii.mesh) : QString();
    ctx.note        = QStringLiteral("自检生成");
    ctx.appVersion  = QCoreApplication::applicationVersion();

    const QString pdfPath = QDir(outDir).filePath(QStringLiteral("report.pdf"));
    QString error;
    const bool pdfOk = writeReportPdf(pdfPath, ctx, &error);
    check(pdfOk && QFileInfo(pdfPath).size() > 20000, QStringLiteral("export-pdf"),
          pdfOk ? QStringLiteral("%1 KB").arg(QFileInfo(pdfPath).size() / 1024)
                : error);

    // 结论页必须一页：多一页就是排版回归（曾因云图改大而溢出）
    int pdfPages = -1;
    {
        QFile pdfFile(pdfPath);
        if (pdfFile.open(QIODevice::ReadOnly)) {
            pdfPages = pdfPageCount(pdfFile.readAll());
            pdfFile.close();
        }
    }
    check(pdfPages == 1, QStringLiteral("export-pdf-single-page"),
          QStringLiteral("%1 页").arg(pdfPages));

    const QString pngPath = QDir(outDir).filePath(QStringLiteral("stress.png"));
    const bool pngOk = writeImagePng(pngPath, watermarked, &error);
    check(pngOk && QFileInfo(pngPath).size() > 5000, QStringLiteral("export-png"),
          pngOk ? QStringLiteral("%1 KB").arg(QFileInfo(pngPath).size() / 1024) : error);
    check(watermarked.size() == shot.size() && watermarked != shot,
          QStringLiteral("watermark-trial"), QStringLiteral("试用水印已叠加"));

    // 用同一份 HTML 渲染一张预览图：PDF 里的字形是否正确，看这张图最准
    {
        QTextDocument preview;
        preview.setDefaultFont(QFont(QStringLiteral("Microsoft YaHei"), 9));
        preview.addResource(QTextDocument::ImageResource, QUrl(QStringLiteral("quickgear:stress.png")),
                            shot);
        preview.setHtml(buildReportHtml(ctx));
        preview.setTextWidth(760);

        const int height = int(preview.size().height()) + 40;
        QImage page(800, height, QImage::Format_ARGB32_Premultiplied);
        page.fill(Qt::white);
        QPainter painter(&page);
        painter.translate(20, 20);
        preview.drawContents(&painter);
        painter.end();

        const QString previewPath = QDir(outDir).filePath(QStringLiteral("report_preview.png"));
        QString previewError;
        const double ink = coverage(page, false);
        check(ink > 0.03 && writeImagePng(previewPath, page, &previewError),
              QStringLiteral("report-html-render"),
              QStringLiteral("墨迹占比 %1% · 高 %2 px").arg(ink * 100.0, 0, 'f', 1).arg(height));
    }

    // ---- 4. 工程文件往返 ----
    ProjectData saved;
    saved.params    = params;
    saved.hasResult = true;
    saved.result    = result;
    saved.meshPath  = asciiPath;
    saved.note      = QStringLiteral("自检工况");
    const QString projectPath = QDir(outDir).filePath(QStringLiteral("selftest.qgpj"));
    const bool saveOk = ::saveProject(projectPath, saved, &error);

    ProjectData loaded;
    const bool loadOk = saveOk && loadProject(projectPath, &loaded, &error);
    check(saveOk && loadOk, QStringLiteral("project-roundtrip"),
          saveOk ? (loadOk ? QStringLiteral("%1 KB").arg(QFileInfo(projectPath).size() / 1024) : error)
                 : error);
    if (loadOk) {
        const bool same = loaded.params.teeth == params.teeth
                          && loaded.params.material == params.material
                          && loaded.hasResult
                          && std::fabs(loaded.result.peakVonMises - result.peakVonMises) < 1e-6
                          && loaded.result.toothStress.size() == result.toothStress.size()
                          && loaded.meshPath == asciiPath
                          && loaded.note == saved.note;
        check(same, QStringLiteral("project-fields"),
              QStringLiteral("σv=%1 · 齿数 %2 · 备注 %3")
                  .arg(loaded.result.peakVonMises, 0, 'f', 1)
                  .arg(loaded.params.teeth)
                  .arg(loaded.note));
    }

    // ---- 5. 授权：签发能通过、改一个字节就不通过 ----
    const QString canonical = LicenseManager::canonicalString(
        QStringLiteral("AAAA-BBBB-CCCC-DDDD-EEEE-FFFF-0000-1111"),
        QStringLiteral("self-test"), QStringLiteral("pro"),
        QDate(2026, 1, 1), QDate(2027, 1, 1));
    const QString signature =
        QString::fromLatin1(LicenseManager::signPayload(canonical).toBase64());
    check(LicenseManager::verifySignature(canonical, signature.toLatin1()),
          QStringLiteral("license-verify"),
          QStringLiteral("签名长度 %1").arg(signature.size()));

    const QString tampered = canonical + QStringLiteral("x");
    check(!LicenseManager::verifySignature(tampered, signature.toLatin1()),
          QStringLiteral("license-tamper"),
          QStringLiteral("篡改规范化串被拒"));

    // ---- 汇总 ----
    g_out << Qt::endl
          << (g_failures == 0 ? "RESULT: ALL PASS" : QStringLiteral("RESULT: %1 FAILED").arg(g_failures))
          << Qt::endl;
    if (g_failures == 0) {
        g_out << "artifacts: " << QDir::toNativeSeparators(pdfPath) << ", "
              << QDir::toNativeSeparators(pngPath) << ", "
              << QDir::toNativeSeparators(projectPath) << Qt::endl;
    }
    return g_failures == 0 ? 0 : 1;
}
