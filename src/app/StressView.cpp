#include "StressView.h"

#include <QFontMetrics>
#include <QLinearGradient>
#include <QLocale>
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>
#include <QRadialGradient>
#include <QtMath>

#include <algorithm>

namespace {

/** 与网页端一致的应力色标（低 → 高） */
const QColor kRamp[] = {
    QColor(0x2B, 0x4C, 0x7E),
    QColor(0x1F, 0x6E, 0x8C),
    QColor(0x2E, 0x8B, 0x57),
    QColor(0xC9, 0xB4, 0x3A),
    QColor(0xD1, 0x77, 0x3A),
    QColor(0xB7, 0x35, 0x2F),
};
const int kRampSize = int(sizeof(kRamp) / sizeof(kRamp[0]));
const double kPi = 3.14159265358979323846;

/** 极坐标点（Qt 画布 y 向下，这里按数学坐标取点） */
QPointF polar(const QPointF &center, double radius, double degrees)
{
    const double rad = degrees * kPi / 180.0;
    return QPointF(center.x() + radius * qCos(rad), center.y() - radius * qSin(rad));
}

} // namespace

StressView::StressView(QWidget *parent)
    : QWidget(parent)
{
    setMinimumSize(420, 340);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setAutoFillBackground(false);
}

void StressView::setGear(int teeth, int pinionTeeth, double module)
{
    m_teeth       = qMax(6, teeth);
    m_pinionTeeth = qMax(6, pinionTeeth);
    m_module      = module;
    update();
}

void StressView::setResult(const SolveResult &result)
{
    m_solved      = result.ok;
    m_ceiling     = result.stressCeiling > 0.0 ? result.stressCeiling : 320.0;
    m_toothStress = result.toothStress;
    update();
}

void StressView::clearResult()
{
    m_solved = false;
    m_toothStress.clear();
    update();
}

void StressView::setDemoMode(bool demo)
{
    m_demo = demo;
    update();
}

void StressView::setMesh(const StlMesh &mesh)
{
    m_mesh      = mesh;
    m_meshCache = QImage();
    update();
}

void StressView::clearMesh()
{
    m_mesh      = StlMesh();
    m_meshCache = QImage();
    update();
}

bool StressView::hasMesh() const
{
    return !m_mesh.isEmpty();
}

QColor StressView::rampColor(double t) const
{
    const double clamped = qBound(0.0, t, 1.0);
    const double span    = clamped * (kRampSize - 1);
    const int    index   = qMin(int(span), kRampSize - 2);
    const double frac    = span - index;
    const QColor &a = kRamp[index];
    const QColor &b = kRamp[index + 1];
    return QColor(int(a.red()   + (b.red()   - a.red())   * frac),
                  int(a.green() + (b.green() - a.green()) * frac),
                  int(a.blue()  + (b.blue()  - a.blue())  * frac));
}

void StressView::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);

    // 深色视口背景，与网页端保持一致
    painter.fillRect(rect(), QColor(0x0E, 0x15, 0x1C));

    const int legendWidth = 78;
    QRectF area = QRectF(rect()).adjusted(16, 16, -legendWidth - 16, -16);
    if (area.width() < 40 || area.height() < 40)
        return;

    // 色标只在出结果后才有意义：没有应力场却挂一根色标，等于在暗示不存在的数据
    if (m_solved)
        drawLegend(painter, QRectF(rect()).adjusted(rect().width() - legendWidth - 8, 16, -8, -16));

    // 结果优先：有结果就画云图，否则画导入几何的预览。
    // 标注只在预览态出现：出结果时画面是参数化齿轮，标一行 STL 尺寸会让人误解。
    if (!m_mesh.isEmpty() && !m_solved) {
        drawMesh(painter, area);
        drawMeshCaption(painter, area);
    } else {
        drawGear(painter, area);
    }

    // 示意标记
    if (m_demo) {
        const QString badge = QStringLiteral("示意数据 · MOCK");
        QFont f = font();
        f.setPointSizeF(8.0);
        painter.setFont(f);
        const QFontMetrics fm(f);
        const QRectF badgeRect(12, 12, fm.horizontalAdvance(badge) + 16, fm.height() + 8);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0xB7, 0x35, 0x2F, 60));
        painter.drawRoundedRect(badgeRect, 3, 3);
        painter.setPen(QColor(0xE8, 0xA0, 0x9A));
        painter.drawText(badgeRect, Qt::AlignCenter, badge);
    }
}

void StressView::drawGear(QPainter &painter, const QRectF &area)
{
    const QPointF center = area.center();
    const double  radius = qMin(area.width(), area.height()) / 2.0 * 0.88;
    const double  rRoot  = radius * 0.82;   // 齿根圆
    const double  rHole  = radius * 0.26;   // 轴孔

    const QRectF outerRect(center.x() - radius, center.y() - radius, radius * 2, radius * 2);
    const QRectF rootRect(center.x() - rRoot, center.y() - rRoot, rRoot * 2, rRoot * 2);
    const QRectF holeRect(center.x() - rHole, center.y() - rHole, rHole * 2, rHole * 2);

    // 底盘：未求解时是一块金属灰，求解后由各齿扇区覆盖
    if (!m_solved) {
        QRadialGradient metal(center - QPointF(radius * 0.25, radius * 0.3), radius * 1.4);
        metal.setColorAt(0.0, QColor(0xB2, 0xBB, 0xC4));
        metal.setColorAt(0.6, QColor(0x8D, 0x97, 0xA3));
        metal.setColorAt(1.0, QColor(0x6B, 0x75, 0x80));
        painter.setPen(Qt::NoPen);
        painter.setBrush(metal);
        painter.drawEllipse(rootRect);
    } else {
        painter.setPen(Qt::NoPen);
        painter.setBrush(kRamp[0]);
        painter.drawEllipse(rootRect);
    }

    const double span = 360.0 / double(m_teeth);
    for (int i = 0; i < m_teeth; ++i) {
        const double start = i * span - 90.0;
        const double end   = start + span;

        QPainterPath sector;
        sector.arcMoveTo(rootRect, start);
        sector.arcTo(rootRect, start, span);
        sector.arcTo(holeRect, end, -span);
        sector.closeSubpath();

        // 齿廓：齿根两侧 → 齿顶两侧，梯形近似（真实客户端用渐开线离散）
        QPolygonF tooth;
        tooth << polar(center, rRoot, start + span * 0.20)
              << polar(center, radius, start + span * 0.34)
              << polar(center, radius, start + span * 0.66)
              << polar(center, rRoot, start + span * 0.80);
        sector.addPolygon(tooth);

        QColor color;
        if (m_solved && !m_toothStress.isEmpty()) {
            const double stress = m_toothStress.at(i % m_toothStress.size());
            color = rampColor(stress / m_ceiling);
        } else {
            color = QColor(0x8D, 0x97, 0xA3);
        }

        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        painter.drawPath(sector);
    }

    // 轴孔与轮缘
    painter.setBrush(QColor(0x0E, 0x15, 0x1C));
    painter.drawEllipse(holeRect);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(QColor(255, 255, 255, 26), 1.0));
    painter.drawEllipse(holeRect);
    painter.drawEllipse(rootRect);

    // 未求解提示
    if (!m_solved) {
        QFont f = font();
        f.setPointSizeF(9.0);
        painter.setFont(f);
        painter.setPen(QColor(0x7D, 0x8B, 0x99));
        const QRectF textRect(center.x() - 120, center.y() - 10, 240, 20);
        painter.drawText(textRect, Qt::AlignCenter, QStringLiteral("点击「运行仿真」生成应力云图"));
    }
}

void StressView::drawMesh(QPainter &painter, const QRectF &area)
{
    const QSize size(qMax(1, int(area.width())), qMax(1, int(area.height())));

    // 投影只跟几何与视口尺寸有关，缓存住避免每帧重排两万个三角形
    if (m_meshCache.size() != size) {
        QImage canvas(size, QImage::Format_ARGB32_Premultiplied);
        canvas.fill(QColor(0x0E, 0x15, 0x1C));

        QPainter p(&canvas);
        p.setRenderHint(QPainter::Antialiasing, true);

        const QVector3D center = (m_mesh.minCorner + m_mesh.maxCorner) * 0.5f;
        const double    extent = qMax(1e-6, m_mesh.maxExtent());
        const double    scale  = qMin(size.width(), size.height()) * 0.74 / extent;

        // 等轴测投影：x' = (dx-dy)cos30，y' = (dx+dy)sin30 - dz
        const auto project = [&](const QVector3D &v) {
            const double dx = double(v.x() - center.x());
            const double dy = double(v.y() - center.y());
            const double dz = double(v.z() - center.z());
            return QPointF(size.width() * 0.5 + (dx - dy) * 0.8660254 * scale,
                           size.height() * 0.5 + ((dx + dy) * 0.5 - dz) * scale);
        };
        const auto depth = [](const StlTriangle &tri) {
            const QVector3D c = (tri.a + tri.b + tri.c) / 3.0f;
            return double(c.x() + c.y() + c.z());
        };

        const QVector3D light = QVector3D(0.35f, -0.62f, 0.70f).normalized();

        const int stride = m_mesh.count() > 24000 ? (m_mesh.count() / 24000 + 1) : 1;
        QVector<int> order;
        order.reserve(m_mesh.count() / stride + 1);
        for (int i = 0; i < m_mesh.count(); i += stride)
            order.append(i);
        std::sort(order.begin(), order.end(), [&](int lhs, int rhs) {
            return depth(m_mesh.triangles.at(lhs)) < depth(m_mesh.triangles.at(rhs));
        });

        p.setPen(Qt::NoPen);
        for (int index : order) {
            const StlTriangle &tri = m_mesh.triangles.at(index);
            const double lambert = qMax(0.0, double(QVector3D::dotProduct(tri.normal, light)));
            const int    shade   = int(58.0 + 150.0 * lambert);
            p.setBrush(QColor(shade, qMin(255, shade + 5), qMin(255, shade + 11), 240));

            QPolygonF poly;
            poly << project(tri.a) << project(tri.b) << project(tri.c);
            p.drawPolygon(poly);
        }
        p.end();
        m_meshCache = canvas;
    }

    painter.drawImage(area.topLeft(), m_meshCache);
}

void StressView::drawMeshCaption(QPainter &painter, const QRectF &area)
{
    QFont f = font();
    f.setPointSizeF(8.0);
    painter.setFont(f);

    // 说清楚边界：几何只做预览，求解仍是参数化齿轮，不假装是"导入即算"
    const QVector3D extent = m_mesh.size();
    const QString full = QStringLiteral("%1 面 · %2×%3×%4 mm · 仅预览，不参与求解")
                             .arg(QLocale().toString(m_mesh.count()),
                                  QString::number(extent.x(), 'f', 1),
                                  QString::number(extent.y(), 'f', 1),
                                  QString::number(extent.z(), 'f', 1));
    const QFontMetrics fm(f);
    const QString caption = fm.elidedText(full, Qt::ElideRight, int(area.width()) - 12);

    // 齿轮盘会占满视口，给文字垫一条半透明底带，避免压在云图上读不清
    const QRectF strip(area.left(), area.bottom() - 26.0, area.width(), 20.0);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0x0E, 0x15, 0x1C, 205));
    painter.drawRect(strip);

    painter.setPen(QColor(0x9A, 0xA8, 0xB5));
    painter.drawText(strip, Qt::AlignCenter, caption);
}

void StressView::drawLegend(QPainter &painter, const QRectF &area)
{
    QFont f = font();
    f.setPointSizeF(8.0);
    painter.setFont(f);

    const QRectF bar(area.left(), area.top() + 18, 10, area.height() - 40);
    QLinearGradient gradient(bar.bottomLeft(), bar.topLeft());
    for (int i = 0; i < kRampSize; ++i)
        gradient.setColorAt(double(i) / (kRampSize - 1), kRamp[i]);

    painter.setPen(Qt::NoPen);
    painter.setBrush(gradient);
    painter.drawRect(bar);

    painter.setPen(QColor(0x8A, 0x97, 0xA6));
    painter.drawText(QRectF(area.left() - 10, area.top() - 6, 74, 16),
                     Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("σv / MPa"));

    const double maxValue = m_solved ? m_ceiling : 320.0;
    const int ticks = 4;
    for (int i = 0; i <= ticks; ++i) {
        const double ratio = double(i) / ticks;
        const double y     = bar.bottom() - bar.height() * ratio;
        painter.setPen(QPen(QColor(0x8A, 0x97, 0xA6), 1.0));
        painter.drawLine(QPointF(bar.right(), y), QPointF(bar.right() + 4, y));
        painter.drawText(QRectF(bar.right() + 6, y - 8, area.width() - 16, 16),
                         Qt::AlignLeft | Qt::AlignVCenter,
                         QString::number(maxValue * ratio, 'f', 0));
    }
}
