#include "app/ReportBuilder.h"
#include "app/StressView.h"
#include "core/Ed25519.h"
#include "core/LicenseBatch.h"
#include "core/LicenseManager.h"
#include "core/MockSolverBackend.h"
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
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QTextDocument>
#include <QTextStream>
#include <QUrl>
#include <QVector>

#include <cmath>
#include <cstring>

#ifdef Q_OS_WIN
#  include <windows.h>   // 读 exe 的版本资源（GetFileVersionInfoW / VerQueryValueW）
#endif

/**
 * qgselftest：不依赖鼠标的冒烟自检。
 *
 * 覆盖流程里最容易悄悄坏掉、又最难靠肉眼确认的五段：
 *   STL 解析（ASCII + 二进制） / 求解 / 结论页导出 / 工程文件往返 /
 *   授权（验签 + 到期判定 + 批量签发）
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

/** 原样写字节：构造"带尾巴"和"被截断"这种真实世界里常见的坏文件 */
bool writeRaw(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    return file.write(bytes) == bytes.size();
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
    // 与客户端保持一致，授权目录才能对齐；版本也别写死——
    // 写死过一次，自检生成的结论页页脚一直写着 0.2.0
    QApplication::setApplicationName(QStringLiteral("QtCAE-QuickGear"));
    QApplication::setApplicationVersion(QStringLiteral(QTG_VERSION));
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

    // ---- 1b. STL 的四种"看着像读进来了、其实在瞎猜"的情况 ----
    {
        // 封闭性：上面那个齿轮实体是真封闭的，必须认出来，体积还得对得上解析值。
        // 只断言 volume > 0 是不够的：开口曲面算出来的有向体积也有正数。
        const double segments = 24.0;                       // 12 齿 × 2
        const double delta    = 2.0 * 3.14159265358979323846 / segments;
        const double analyticArea = 0.5 * segments * 40.0 * 30.0 * std::sin(delta);
        const double analyticVolume = analyticArea * 20.0;   // mm³，拉 20 mm

        const double closedErr = ascii.ok
            ? std::fabs(ascii.mesh.volume - analyticVolume) / analyticVolume
            : 1.0;
        check(ascii.ok && ascii.mesh.solidity == MeshSolidity::Closed
                  && ascii.mesh.volumeUsable(),
              QStringLiteral("stl-solidity-closed"),
              ascii.ok ? QStringLiteral("判定=%1 · %2")
                             .arg(solidityName(ascii.mesh.solidity), stlSummary(ascii.mesh))
                       : ascii.error);
        check(closedErr < 0.01, QStringLiteral("stl-volume-analytic"),
              QStringLiteral("实测 %1 cm³ · 解析 %2 cm³ · 偏差 %3%")
                  .arg(ascii.mesh.volume / 1000.0, 0, 'f', 3)
                  .arg(analyticVolume / 1000.0, 0, 'f', 3)
                  .arg(closedErr * 100.0, 0, 'f', 3));

        // 尾部填充：很多导出器会在二进制 STL 末尾补字节，不能因此当 ASCII 去读
        QFile binFile(binPath);
        QByteArray binBytes;
        if (binFile.open(QIODevice::ReadOnly)) {
            binBytes = binFile.readAll();
            binFile.close();
        }
        const QString paddedPath = QDir(outDir).filePath(QStringLiteral("gear_padded.stl"));
        const bool paddedWritten = !binBytes.isEmpty()
            && writeRaw(paddedPath, binBytes + QByteArray("\n\0\0\0\0\0\0\0\0", 9));
        const StlLoadResult padded = paddedWritten ? loadStl(paddedPath) : StlLoadResult();
        check(padded.ok && padded.format == StlLoadResult::Binary
                  && padded.mesh.count() == binary.mesh.count(),
              QStringLiteral("stl-binary-trailing-bytes"),
              padded.ok ? QStringLiteral("尾部多 9 字节，仍按二进制读到 %1 面")
                              .arg(padded.mesh.count())
                        : padded.error);

        // 截断：说"被截断、缺多少字节"，而不是让用户看到"没有可解析的三角形"
        const QString cutPath = QDir(outDir).filePath(QStringLiteral("gear_cut.stl"));
        const bool cutWritten = !binBytes.isEmpty()
            && writeRaw(cutPath, binBytes.left(binBytes.size() / 2));
        const StlLoadResult cut = cutWritten ? loadStl(cutPath) : StlLoadResult();
        check(!cut.ok && cut.error.contains(QStringLiteral("截断")),
              QStringLiteral("stl-binary-truncated"),
              cut.ok ? QStringLiteral("截断文件居然读成功了") : cut.error);

        // 残缺 facet：缺顶点的块必须整块丢弃，不能拿它跟下一块拼三角形
        const QByteArray holey =
            "solid holey\n"
            "  facet normal 0 0 0\n    outer loop\n"
            "      vertex 0 0 0\n      vertex 1 0 0\n      vertex 0 1 0\n"
            "    endloop\n  endfacet\n"
            "  facet normal 0 0 0\n    outer loop\n"
            "      vertex 5 5 5\n      vertex 6 5 5\n"
            "    endloop\n  endfacet\n"
            "  facet normal 0 0 0\n    outer loop\n"
            "      vertex 0 0 0\n      vertex 0 3 0\n      vertex 3 3 0\n"
            "    endloop\n  endfacet\n"
            "endsolid holey\n";
        const QString holeyPath = QDir(outDir).filePath(QStringLiteral("holey.stl"));
        const StlLoadResult holeyLoaded = writeRaw(holeyPath, holey) ? loadStl(holeyPath)
                                                                    : StlLoadResult();
        // 第二个三角形的坐标和 = 0+3+3+3 = 9；若与上一块的残块拼接，
        // 就会变成 (5,5,5)+(6,5,5)+(0,0,0) 的 31，两个值差得很开
        double secondSum = -1.0;
        if (holeyLoaded.ok && holeyLoaded.mesh.count() >= 2) {
            const StlTriangle &t = holeyLoaded.mesh.triangles.at(1);
            secondSum = double(t.a.x() + t.a.y() + t.a.z()) + double(t.b.x() + t.b.y() + t.b.z())
                      + double(t.c.x() + t.c.y() + t.c.z());
        }
        check(holeyLoaded.ok && holeyLoaded.mesh.count() == 2
                  && holeyLoaded.mesh.malformedFacets == 1 && std::fabs(secondSum - 9.0) < 0.01,
              QStringLiteral("stl-ascii-malformed-facet"),
              holeyLoaded.ok
                  ? QStringLiteral("保留 %1 面 · 丢弃 %2 个残缺面 · 次面坐标和 %3")
                        .arg(holeyLoaded.mesh.count())
                        .arg(holeyLoaded.mesh.malformedFacets)
                        .arg(secondSum, 0, 'f', 1)
                  : holeyLoaded.error);

        // 开口网格：少一块面就不再是实体，体积必须报"不适用"而不是给个数
        const QString openPath = QDir(outDir).filePath(QStringLiteral("gear_open.stl"));
        const bool openWritten = writeBinaryStl(openPath, gear.mid(0, gear.size() - 1));
        const StlLoadResult openMesh = openWritten ? loadStl(openPath) : StlLoadResult();
        const QString openSummary = openMesh.ok ? stlSummary(openMesh.mesh) : QString();
        check(openMesh.ok && openMesh.mesh.solidity == MeshSolidity::Open
                  && !openMesh.mesh.volumeUsable()
                  && openSummary.contains(QStringLiteral("非封闭")),
              QStringLiteral("stl-open-no-volume"),
              openMesh.ok ? QStringLiteral("判定=%1 · %2")
                                .arg(solidityName(openMesh.mesh.solidity), openSummary)
                          : openMesh.error);
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

    // ---- 2b. 网页端与客户端的公式口径必须一致 ----
    // 两边各写一份经验式，迟早一边 350 一边 400；同一个参数在网页上给 n=2.92、
    // 装完客户端变 3.33，是最容易被用户截图对比出来的一种不可信。
    // 这里直接读 docs/index.html 的常量对账：改了一边没改另一边，自检会报。
    {
        const QString pagePath = QDir(QString::fromLatin1(QTG_SOURCE_DIR))
                                     .filePath(QStringLiteral("docs/index.html"));
        QFile page(pagePath);
        const auto numberAfter = [](const QString &text, const QString &key, bool *ok) {
            const int at = text.indexOf(key);
            if (at < 0) {
                *ok = false;
                return 0.0;
            }
            int i = at + key.size();
            while (i < text.size() && (text.at(i) == QLatin1Char(' ') || text.at(i) == QLatin1Char(':')))
                ++i;
            int begin = i;
            while (i < text.size()
                   && (text.at(i).isDigit() || text.at(i) == QLatin1Char('.')))
                ++i;
            bool converted = false;
            const double value = text.mid(begin, i - begin).toDouble(&converted);
            *ok = i > begin && converted;
            return value;
        };

        if (!page.open(QIODevice::ReadOnly)) {
            check(true, QStringLiteral("web-formula-parity"),
                  QStringLiteral("未找到 docs/index.html（非源码树运行，跳过）"));
        } else {
            const QString html = QString::fromUtf8(page.readAll());
            page.close();

            bool ceilingOk = false;
            bool allowOk   = false;
            const double pageCeiling = numberAfter(html, QStringLiteral("allowStressMpa"), &ceilingOk);
            const double pageAllow   = numberAfter(html, QStringLiteral("materialAllow"), &allowOk);
            const double clientAllow = allowableStressMPa(params.material);
            // 截断规则与单元数取整也得一致：只对齐常量不够
            const bool capSame   = html.contains(QStringLiteral("Math.min(raw, CONFIG.allowStressMpa * 2)"));
            const bool floorSame = html.contains(QStringLiteral("Math.floor(1.8e4"));

            check(ceilingOk && allowOk && qAbs(pageCeiling - result.stressCeiling) < 1e-6
                      && qAbs(pageAllow - clientAllow) < 1e-6 && capSame && floorSame,
                  QStringLiteral("web-formula-parity"),
                  QStringLiteral("量程 %1 vs %2 · 许用 %3 vs %4 · 截断 %5 · 取整 %6")
                      .arg(pageCeiling, 0, 'f', 0)
                      .arg(result.stressCeiling, 0, 'f', 0)
                      .arg(pageAllow, 0, 'f', 0)
                      .arg(clientAllow, 0, 'f', 0)
                      .arg(capSame ? QStringLiteral("一致") : QStringLiteral("不一致"))
                      .arg(floorSame ? QStringLiteral("一致") : QStringLiteral("不一致")));
        }
    }

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

    // 页脚得能让人自己核对签名算法：客户拿着一份 PDF 就能看出是不是公钥签的
    {
        ReportContext licensedCtx = ctx;
        licensedCtx.licensed   = true;
        licensedCtx.licensee   = QStringLiteral("自检客户");
        licensedCtx.licenseAlg = QStringLiteral("ed25519");
        const QString licensedHtml = buildReportHtml(licensedCtx);
        const QString trialHtml    = buildReportHtml(ctx);
        check(licensedHtml.contains(QStringLiteral("签名 ed25519"))
                  && trialHtml.contains(QStringLiteral("试用版导出"))
                  && !trialHtml.contains(QStringLiteral("签名 ed25519")),
              QStringLiteral("report-license-alg"),
              QStringLiteral("已授权页脚带签名算法 · 试用页脚不冒充授权"));
    }

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

    // ---- 6. 授权：到期判定与批量签发 ----
    const QDate  today     = QDate::currentDate();
    const QString ownMachine = LicenseManager::machineCode();
    const QString fakeMachine = QStringLiteral("3F1B-92C4-7E10-AB35-6612-0D8C-4F5E-2299");

    // 客户经常把机器码抄成一行、还带小写；归一化必须把它们认成同一个码，
    // 否则签出去的授权客户装上去只会看到“与本机不匹配”
    const QString written = LicenseManager::normalizeMachineCode(
        QStringLiteral("3f1b92c47e10ab3566120d8c4f5e2299"));
    check(written == fakeMachine && LicenseManager::machineCodeLooksValid(written)
              && !LicenseManager::machineCodeLooksValid(QStringLiteral("3F1B-92C4-7E10")),
          QStringLiteral("license-normalize"),
          QStringLiteral("%1 · 短码被拒").arg(written));

    const QString licenseDir  = QDir(outDir).filePath(QStringLiteral("issued"));
    const QString expiringPath = QDir(licenseDir).filePath(QStringLiteral("self-expiring.qglic"));
    const QString expiredPath  = QDir(licenseDir).filePath(QStringLiteral("self-expired.qglic"));
    QDir().mkpath(licenseDir);
    LicenseManager::writeLicenseFile(expiringPath, ownMachine, QStringLiteral("自检客户"),
                                     QStringLiteral("pro"), today, today.addDays(10));
    LicenseManager::writeLicenseFile(expiredPath, ownMachine, QStringLiteral("自检客户"),
                                     QStringLiteral("pro"), today.addDays(-30), today.addDays(-3));

    const LicenseInfo expiring = LicenseManager::parseFile(expiringPath, true);
    check(expiring.valid && expiring.daysLeft == 10 && expiring.expiringSoon(),
          QStringLiteral("license-expiry-window"),
          QStringLiteral("剩 %1 天 · %2").arg(expiring.daysLeft).arg(expiring.message));

    // 过期必须盖过“机器匹配”：否则过期一个月的老客户依旧能用
    const LicenseInfo overdue = LicenseManager::parseFile(expiredPath, true);
    check(!overdue.valid && overdue.state == LicenseState::Expired && overdue.daysLeft == -3,
          QStringLiteral("license-expired-state"),
          QStringLiteral("STATE=%1 · %2")
              .arg(LicenseManager::stateName(overdue.state), overdue.message));

    // 审计他机授权：跳过机器绑定才能看到到期日，但签名仍然要验
    const QString foreignPath = QDir(licenseDir).filePath(QStringLiteral("self-foreign.qglic"));
    LicenseManager::writeLicenseFile(foreignPath, fakeMachine, QStringLiteral("客户机器"),
                                     QStringLiteral("pro"), today, today.addDays(200));
    const LicenseInfo onOtherMachine = LicenseManager::parseFile(foreignPath, true);
    const LicenseInfo asAudited      = LicenseManager::parseFile(foreignPath, false);
    check(onOtherMachine.state == LicenseState::ForeignMachine
              && onOtherMachine.signatureChecked && asAudited.valid
              && asAudited.boundMachine == fakeMachine,
          QStringLiteral("license-audit-foreign"),
          QStringLiteral("本机 %1 · 审计 %2")
              .arg(LicenseManager::stateName(onOtherMachine.state),
                   LicenseManager::stateName(asAudited.state)));

    // ---- 6b. 导入：坏文件不能把手上能用的授权顶掉 ----
    const QString installDir    = QDir(outDir).filePath(QStringLiteral("install"));
    const QString installTarget = QDir(installDir).filePath(QStringLiteral("license.qglic"));
    QDir().mkpath(installDir);
    QFile::remove(installTarget);

    const auto readTarget = [&installTarget]() -> QByteArray {
        QFile f(installTarget);
        if (!f.open(QIODevice::ReadOnly))
            return QByteArray();
        return f.readAll();
    };

    LicenseManager installer;
    QString        installError;
    const bool     installed = installer.installFrom(expiringPath, &installError, installTarget);
    const LicenseInfo installedInfo = LicenseManager::parseFile(installTarget, true);
    check(installed && installError.isEmpty() && installedInfo.valid && installedInfo.daysLeft == 10,
          QStringLiteral("license-install-ok"),
          QStringLiteral("目标%1 · 剩 %2 天")
              .arg(QFileInfo::exists(installTarget) ? QStringLiteral("已写入") : QStringLiteral("缺失"))
              .arg(installedInfo.daysLeft));

    // 后续两个“应当被拒”的用例必须证明目标文件一个字节都没变
    const QByteArray targetBefore = readTarget();

    QString    rejectError;
    const bool rejectedForeign = !installer.installFrom(foreignPath, &rejectError, installTarget);
    const bool intactAfterForeign = (readTarget() == targetBefore);
    check(rejectedForeign && intactAfterForeign
              && rejectError.contains(QStringLiteral("未通过校验")),
          QStringLiteral("license-install-reject"),
          QStringLiteral("他机授权被拒 · 原文件%1")
              .arg(intactAfterForeign ? QStringLiteral("未变") : QStringLiteral("被改")));

    // 错点旧邮件附件是真实发生的事：拿过期件覆盖有效件必须拦住
    QString    staleError;
    const bool rejectedStale = !installer.installFrom(expiredPath, &staleError, installTarget);
    const bool intactAfterStale = (readTarget() == targetBefore);
    check(rejectedStale && intactAfterStale
              && staleError.contains(QStringLiteral("已过期")),
          QStringLiteral("license-install-keep-valid"),
          QStringLiteral("过期件未覆盖有效件 · 原文件%1")
              .arg(intactAfterStale ? QStringLiteral("未变") : QStringLiteral("被改")));

    // ---- 6c. Ed25519：RFC 8032 §7.1 官方向量（自己写的曲线不拿官方向量对一遍不算数）----
    struct EdCase
    {
        const char *seed;
        const char *pub;
        const char *msgHex;
        const char *sig;
    };
    const EdCase edCases[] = {
        {"9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
         "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", "",
         "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e06522490155"
         "5fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b"},
        {"4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb",
         "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c", "72",
         "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da"
         "085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00"},
        {"c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7",
         "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025", "af82",
         "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac"
         "18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a"},
    };

    int  edKeyOk = 0;
    int  edSignOk = 0;
    bool edAllVerify = true;
    for (const EdCase &c : edCases) {
        const QByteArray seed = QByteArray::fromHex(c.seed);
        const QByteArray msg = QByteArray::fromHex(c.msgHex);
        const QByteArray pub = QByteArray::fromHex(c.pub);
        const QByteArray sig = QByteArray::fromHex(c.sig);

        QByteArray derived;
        if (Ed25519::derivePublicKey(seed, &derived) && derived == pub)
            ++edKeyOk;

        QByteArray produced;
        if (Ed25519::sign(msg, seed, &produced) && produced == sig)
            ++edSignOk;

        if (!Ed25519::verify(msg, sig, pub))
            edAllVerify = false;
    }
    check(edKeyOk == 3, QStringLiteral("ed25519-rfc8032-key"),
          QStringLiteral("公钥推导 %1/3 与官方向量一致").arg(edKeyOk));
    check(edSignOk == 3, QStringLiteral("ed25519-rfc8032-sign"),
          QStringLiteral("签名 %1/3 逐字节一致").arg(edSignOk));

    // 改一个 bit 就必须被拒：这也是“客户端只有公钥也签不出东西”的前提
    {
        const QByteArray seed = QByteArray::fromHex(edCases[1].seed);
        const QByteArray msg = QByteArray::fromHex(edCases[1].msgHex);
        const QByteArray pub = QByteArray::fromHex(edCases[1].pub);
        const QByteArray sig = QByteArray::fromHex(edCases[1].sig);

        QByteArray badMsg = msg;
        badMsg[0] = static_cast<char>(badMsg[0] ^ 0x01);
        QByteArray badSig = sig;
        badSig[10] = static_cast<char>(badSig[10] ^ 0x01);
        QByteArray badPub = pub;
        badPub[5] = static_cast<char>(badPub[5] ^ 0x01);
        QByteArray shortSig = sig.left(63);

        const bool rejects = edAllVerify && !Ed25519::verify(badMsg, sig, pub)
                             && !Ed25519::verify(msg, badSig, pub)
                             && !Ed25519::verify(msg, sig, badPub)
                             && !Ed25519::verify(msg, shortSig, pub);
        check(rejects, QStringLiteral("ed25519-tamper"),
              QStringLiteral("改消息/改签名/换公钥/截短签名均被拒"));
    }

    // ---- 6d. 授权文件层面的 Ed25519 往返 + 旧 HMAC 文件仍可识别 ----
    // 上面测的是曲线本身，这里测的是“写进文件 → 读回来 → 验签”这条链，
    // 包括 alg 字段分派：分派写错时曲线全对也照样全部授权失效。
    {
        const QDate  issuedOn = today;
        const QDate  expireOn = today.addDays(90);
        const QString edMachine = LicenseManager::machineCode();
        const QString edLicensee = QStringLiteral("Ed25519 往返客户");
        const QString edEdition = QStringLiteral("pro");

        // 固定种子便于失败时重现；pubB 只是“另一把公钥”，用来证明换钥即失效
        const QByteArray seedA = QByteArray::fromHex(edCases[1].seed);
        QByteArray       pubA;
        QByteArray       pubB;
        const bool keyOk = Ed25519::derivePublicKey(seedA, &pubA)
                           && Ed25519::derivePublicKey(Ed25519::randomSeed(), &pubB) && pubA != pubB;

        const QString edPath = QDir(licenseDir).filePath(QStringLiteral("self-ed25519.qglic"));
        QString       edSig;
        QString       edErr;
        const bool    edWritten = LicenseManager::writeLicenseFile(
            edPath, edMachine, edLicensee, edEdition, issuedOn, expireOn, &edSig, &edErr, seedA);

        // 从落盘的真实内容重建规范化串，而不是拿内存里那份——判的就是文件本身
        QString   algInFile;
        int       verInFile = -1;
        QString   sigInFile;
        QDate     issuedInFile;
        QDate     expireInFile;
        QString   licenseeInFile;
        QString   editionInFile;
        QString   machineInFile;
        {
            QFile f(edPath);
            if (f.open(QIODevice::ReadOnly)) {
                const QJsonObject obj = QJsonDocument::fromJson(f.readAll()).object();
                f.close();
                algInFile      = obj.value(QStringLiteral("alg")).toString();
                verInFile      = obj.value(QStringLiteral("v")).toInt();
                sigInFile      = obj.value(QStringLiteral("sig")).toString();
                licenseeInFile = obj.value(QStringLiteral("licensee")).toString();
                editionInFile  = obj.value(QStringLiteral("edition")).toString();
                machineInFile  = obj.value(QStringLiteral("machine")).toString();
                issuedInFile   = QDate::fromString(obj.value(QStringLiteral("issued")).toString(),
                                                   Qt::ISODate);
                expireInFile   = QDate::fromString(obj.value(QStringLiteral("expire")).toString(),
                                                   Qt::ISODate);
            }
        }
        const QString canonicalOnDisk = LicenseManager::canonicalString(
            machineInFile, licenseeInFile, editionInFile, issuedInFile, expireInFile);
        const QByteArray sigABytes = QByteArray::fromBase64(sigInFile.toLatin1());

        const bool edRoundTrip = edWritten && edErr.isEmpty() && algInFile == QLatin1String("ed25519")
                                 && verInFile == 2 && sigABytes.size() == 64
                                 && edSig == sigInFile
                                 && LicenseManager::verifyEd25519With(pubA, canonicalOnDisk, sigABytes);
        check(edRoundTrip, QStringLiteral("license-ed25519-roundtrip"),
              QStringLiteral("落盘 ALG=%1 v=%2 · 用对应公钥验签%3")
                  .arg(algInFile)
                  .arg(verInFile)
                  .arg(edRoundTrip ? QStringLiteral("通过") : QStringLiteral("失败")));

        // 换一把公钥必须验不过：这就是“客户端只带公钥”能挡住伪造的全部依赖
        const LicenseInfo fromDisk = LicenseManager::parseFile(edPath, true);
        const bool        wrongKeyOk =
            keyOk && !LicenseManager::verifyEd25519With(pubB, canonicalOnDisk, sigABytes);
        // 这份文件是用 pubA 对应的种子签的：内置公钥恰好是 pubA 时它应该有效，
        // 否则必须明确报“签名无效”。两种情况下都不允许“公钥不匹配却放行”。
        const bool embeddedIsA = LicenseManager::signingPublicKey() == pubA;
        const bool gateHolds   = embeddedIsA ? fromDisk.valid
                                             : fromDisk.state == LicenseState::BadSignature;
        check(wrongKeyOk && gateHolds, QStringLiteral("license-ed25519-wrongkey"),
              QStringLiteral("换公钥即失效 · 内置公钥%1 · STATE=%2")
                  .arg(embeddedIsA ? QStringLiteral("匹配") : QStringLiteral("不匹配"),
                       LicenseManager::stateName(fromDisk.state)));

        // 旧授权（HMAC、无 alg 字段）必须继续可用，否则一次升级把所有老客户关在门外
        const QString hmacPath = QDir(licenseDir).filePath(QStringLiteral("self-hmac.qglic"));
        QString       hmacErr;
        const bool    hmacWritten = LicenseManager::writeLicenseFile(
            hmacPath, edMachine, QStringLiteral("旧授权客户"), edEdition, issuedOn, expireOn,
            nullptr, &hmacErr);
        const LicenseInfo hmacInfo = LicenseManager::parseFile(hmacPath, true);
        QString algOfHmac;
        int     verOfHmac = -1;
        {
            QFile f(hmacPath);
            if (f.open(QIODevice::ReadOnly)) {
                const QJsonObject obj = QJsonDocument::fromJson(f.readAll()).object();
                f.close();
                algOfHmac = obj.value(QStringLiteral("alg")).toString();
                verOfHmac = obj.value(QStringLiteral("v")).toInt();
            }
        }
        check(hmacWritten && hmacInfo.valid && algOfHmac == QLatin1String("hmac") && verOfHmac == 1,
              QStringLiteral("license-hmac-compat"),
              QStringLiteral("旧算法仍可验 · ALG=%1 v=%2 STATE=%3")
                  .arg(algOfHmac)
                  .arg(verOfHmac)
                  .arg(LicenseManager::stateName(hmacInfo.state)));

        // 未来格式：v 大于本客户端认识的版本时必须说“升级客户端”，
        // 而不是让客户拿着正确的文件看到“签名无效”
        const QString futurePath = QDir(licenseDir).filePath(QStringLiteral("self-future.qglic"));
        {
            QJsonObject obj;
            obj.insert(QStringLiteral("v"), LicenseManager::kFormatVersion + 1);
            obj.insert(QStringLiteral("alg"), QStringLiteral("ed25519"));
            obj.insert(QStringLiteral("machine"), edMachine);
            obj.insert(QStringLiteral("licensee"), QStringLiteral("未来格式"));
            obj.insert(QStringLiteral("edition"), edEdition);
            obj.insert(QStringLiteral("issued"), issuedOn.toString(Qt::ISODate));
            obj.insert(QStringLiteral("expire"), expireOn.toString(Qt::ISODate));
            obj.insert(QStringLiteral("sig"), QString(88, QLatin1Char('A')));

            QFile f(futurePath);
            if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                f.write(QJsonDocument(obj).toJson(QJsonDocument::Indented));
                f.close();
            }
        }
        const LicenseInfo future = LicenseManager::parseFile(futurePath, true);
        check(future.state == LicenseState::Unreadable
                  && future.message.contains(
                      QStringLiteral("v%1").arg(LicenseManager::kFormatVersion + 1))
                  && future.message.contains(QStringLiteral("升级客户端")),
              QStringLiteral("license-format-too-new"), future.message);

        // 续费只能拿验签通过的旧授权：否则等于替伪造者把假授权洗成真的
        const QString tamperedPath =
            QDir(licenseDir).filePath(QStringLiteral("self-tampered-renew.qglic"));
        {
            QFile src(edPath);
            if (src.open(QIODevice::ReadOnly)) {
                QByteArray bytes = src.readAll();
                src.close();
                bytes.replace("Ed25519 往返客户", "Ed25519 往返客尸");   // 改一个字，签名就对不上
                QFile dst(tamperedPath);
                if (dst.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                    dst.write(bytes);
                    dst.close();
                }
            }
        }
        const LicenseInfo tampered = LicenseManager::parseFile(tamperedPath, true);
        const bool notRenewable = !LicenseManager::licenseIsRenewable(tampered)
                                  && !LicenseManager::licenseIsRenewable(LicenseInfo());
        // 可续的三种：有效（本机）、他机、已过期——共同点是验签都过得去。
        // 注意不能用 fromDisk：那份是特地用另一把种子签的，本来就该是 BADSIG。
        const QString foreignPath = QDir(licenseDir).filePath(QStringLiteral("self-foreign-renew.qglic"));
        LicenseManager::writeLicenseFile(foreignPath, QStringLiteral("3F1B-92C4-7E10-AB35-6612-0D8C-4F5E-2299"),
                                         QStringLiteral("他机客户"), edEdition, issuedOn, expireOn);
        const LicenseInfo foreignInfo = LicenseManager::parseFile(foreignPath, true);
        const bool        renewable =
            LicenseManager::licenseIsRenewable(hmacInfo)            // Valid
            && LicenseManager::licenseIsRenewable(foreignInfo)      // ForeignMachine
            && LicenseManager::licenseIsRenewable(LicenseManager::parseFile(expiredPath, true));
        check(notRenewable && renewable, QStringLiteral("license-renewable-gate"),
              QStringLiteral("伪造/空文件不得续费 · 有效/他机/过期可续 · 篡改件 STATE=%1 他机 STATE=%2")
                  .arg(LicenseManager::stateName(tampered.state),
                       LicenseManager::stateName(foreignInfo.state)));

        // 续费起算日：提前续费接旧到期日，过期后续从今天算
        const QDate inTenDays = today.addDays(10);
        check(LicenseManager::renewalBase(inTenDays) == inTenDays
                  && LicenseManager::renewalBase(today.addDays(-200)) == today
                  && LicenseManager::renewalBase(QDate()) == today,
              QStringLiteral("license-renewal-base"),
              QStringLiteral("提前续接旧到期日 · 过期从今天算 · 永久从今天算"));
    }

    // 台账：表头 + 注释 + 一行连写小写机器码 + 一行带逗号的公司名 + 一行抄错的码
    const QByteArray ledger = QStringLiteral(
                                  "# 自检台账\n"
                                  "machine,licensee,edition,days,expire\n"
                                  "3f1b92c47e10ab3566120d8c4f5e2299,某某机械厂,pro,365,\n"
                                  "A20A-7BF9-1FA0-0DB1-C5C8-302C-B3D8-A600,\"某某传动, 二分厂\",pro,,2027-06-30\n"
                                  "ZZZZ-0000,机器码抄错了,pro,30,\n")
                                  .toUtf8();

    QStringList              rowErrors;
    const QVector<BatchOrder> orders = LicenseBatch::parseLedger(ledger, &rowErrors);
    check(orders.size() == 2 && rowErrors.size() == 1
              && orders.value(1).licensee == QStringLiteral("某某传动, 二分厂"),
          QStringLiteral("license-batch-ledger"),
          QStringLiteral("可用 %1 行 · 拒 %2 行 · %3")
              .arg(orders.size())
              .arg(rowErrors.size())
              .arg(rowErrors.value(0)));

    const QVector<BatchResult> batch = LicenseBatch::issueAll(orders, licenseDir, false, nullptr);
    int    writtenCount = 0;
    for (const BatchResult &result : batch) {
        if (result.ok() && QFileInfo::exists(result.file))
            ++writtenCount;
    }
    const QString manifestPath = QDir(licenseDir).filePath(QStringLiteral("manifest.csv"));
    int           manifestRows = -1;
    bool          manifestBom  = false;
    QString       manifestHead;
    QString       manifestAlgCell;
    {
        QFile manifest(manifestPath);
        if (manifest.open(QIODevice::ReadOnly)) {
            const QByteArray bytes = manifest.readAll();
            manifest.close();
            manifestRows = bytes.count('\n') - 1;   // 减表头
            manifestBom  = bytes.startsWith("\xEF\xBB\xBF");

            const QStringList lines =
                QString::fromUtf8(bytes.mid(3)).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
            manifestHead = lines.value(0);
            // 第一条数据行的 status 后面那一格就是 alg：交付单要能自证用了哪种算法
            const QStringList firstRow =
                lines.value(1).split(QLatin1Char(','), Qt::KeepEmptyParts);
            manifestAlgCell = firstRow.value(7);
        }
    }
    check(writtenCount == 2 && manifestRows == 2 && manifestBom
              && manifestHead.endsWith(QStringLiteral("status,alg,sig8,error"))
              && manifestAlgCell == QLatin1String("hmac"),
          QStringLiteral("license-batch-issue"),
          QStringLiteral("写出 %1 份 · 交付单 %2 行 · BOM %3 · 算法列=%4")
              .arg(writtenCount)
              .arg(manifestRows)
              .arg(manifestBom ? QStringLiteral("有") : QStringLiteral("无"))
              .arg(manifestAlgCell));

    // dry-run 一旦落了盘就不叫试跑：客户会收到两份不同到期日的授权
    const QString dryDir = QDir(outDir).filePath(QStringLiteral("issued-dry"));
    LicenseBatch::issueAll(orders, dryDir, true, nullptr);
    check(!QDir(dryDir).exists(), QStringLiteral("license-batch-dry-run"),
          QStringLiteral("dry-run 未建目录、未写文件"));

    // ---- 7. 发版物：版本号只有 project(... VERSION ...) 一处来源 ----
    // 写死过一次 "0.2.0"，自检生成的结论页页脚就这么挂了三个月；
    // exe 的版本资源也一样，对不上时用户报问题说不清自己装的是哪版。
#ifdef Q_OS_WIN
    {
        const QString appPath = QDir(QCoreApplication::applicationDirPath())
                                    .filePath(QStringLiteral("QuickGear.exe"));
        QString fileVersion, productVersion, description, readError;
        if (!QFileInfo::exists(appPath)) {
            readError = QStringLiteral("未构建 QuickGear.exe，无法核对版本资源");
        } else {
            const QString nativePath = QDir::toNativeSeparators(appPath);
            DWORD         ignored    = 0;
            const DWORD   size       = ::GetFileVersionInfoSizeW(
                reinterpret_cast<LPCWSTR>(nativePath.utf16()), &ignored);
            QByteArray buffer(int(size), '\0');
            if (size == 0) {
                readError = QStringLiteral("exe 里没有版本资源");
            } else if (!::GetFileVersionInfoW(reinterpret_cast<LPCWSTR>(nativePath.utf16()), 0,
                                              size, buffer.data())) {
                readError = QStringLiteral("版本资源读不出来");
            } else {
                auto query = [&buffer](const wchar_t *key) -> QString {
                    void *value  = nullptr;
                    UINT  length = 0;
                    if (!::VerQueryValueW(buffer.constData(), key, &value, &length) || value == nullptr)
                        return QString();
                    return QString::fromWCharArray(static_cast<const wchar_t *>(value), int(length))
                        .remove(QChar('\0'));
                };
                fileVersion    = query(L"\\StringFileInfo\\080404b0\\FileVersion");
                productVersion = query(L"\\StringFileInfo\\080404b0\\ProductVersion");
                description    = query(L"\\StringFileInfo\\080404b0\\FileDescription");
            }
        }
        check(readError.isEmpty() && fileVersion == QStringLiteral(QTG_VERSION)
                  && productVersion == QStringLiteral(QTG_VERSION) && !description.isEmpty(),
              QStringLiteral("app-version-resource"),
              readError.isEmpty()
                  ? QStringLiteral("FileVersion=%1 · ProductVersion=%2 · 描述=%3")
                        .arg(fileVersion, productVersion, description)
                  : readError);
    }
#endif

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
