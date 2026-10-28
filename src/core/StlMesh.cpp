#include "StlMesh.h"

#include <QByteArray>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QtEndian>
#include <QtMath>

#include <cmath>
#include <cstring>

namespace {

/** 单文件上限：再大就不是"轻量化"工具该吃的模型了，直接拒绝比崩溃体面 */
constexpr int kMaxTriangles = 2000000;

float readFloat(const uchar *p)
{
    float v = 0.0f;
    std::memcpy(&v, p, sizeof(float));
    return v;
}

double readDouble(const char **pp, const char *end, bool *ok)
{
    const char *p = *pp;
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'))
        ++p;

    const char *start = p;
    if (p < end && (*p == '+' || *p == '-'))
        ++p;
    while (p < end && ((*p >= '0' && *p <= '9') || *p == '.'))
        ++p;
    if (p < end && (*p == 'e' || *p == 'E')) {
        ++p;
        if (p < end && (*p == '+' || *p == '-'))
            ++p;
        while (p < end && (*p >= '0' && *p <= '9'))
            ++p;
    }

    *ok  = false;
    *pp  = p;
    if (p == start)
        return 0.0;

    bool converted = false;
    const double value = QString::fromLatin1(start, int(p - start)).toDouble(&converted);
    *ok = converted;
    return converted ? value : 0.0;
}

/** 累加一个三角形：包围盒 + 表面积 + 有向体积 */
void accumulate(StlMesh *mesh, const QVector3D &a, const QVector3D &b, const QVector3D &c)
{
    const QVector3D ab = b - a;
    const QVector3D ac = c - a;
    const QVector3D cross = QVector3D::crossProduct(ab, ac);

    mesh->area += 0.5 * double(cross.length());
    mesh->volume += double(QVector3D::dotProduct(a, QVector3D::crossProduct(b, c))) / 6.0;

    if (mesh->triangles.isEmpty()) {
        mesh->minCorner = a;
        mesh->maxCorner = a;
    }
    const QVector3D pts[3] = {a, b, c};
    for (const QVector3D &p : pts) {
        mesh->minCorner.setX(qMin(mesh->minCorner.x(), p.x()));
        mesh->minCorner.setY(qMin(mesh->minCorner.y(), p.y()));
        mesh->minCorner.setZ(qMin(mesh->minCorner.z(), p.z()));
        mesh->maxCorner.setX(qMax(mesh->maxCorner.x(), p.x()));
        mesh->maxCorner.setY(qMax(mesh->maxCorner.y(), p.y()));
        mesh->maxCorner.setZ(qMax(mesh->maxCorner.z(), p.z()));
    }

    StlTriangle tri;
    tri.a      = a;
    tri.b      = b;
    tri.c      = c;
    tri.normal = cross.lengthSquared() > 0.0f ? cross.normalized() : QVector3D(0.0f, 0.0f, 1.0f);
    mesh->triangles.append(tri);
}

bool parseBinary(const QByteArray &data, StlMesh *mesh, QString *error)
{
    const uchar *base  = reinterpret_cast<const uchar *>(data.constData());
    const quint32 count = qFromLittleEndian<quint32>(base + 80);

    if (count == 0) {
        *error = QStringLiteral("STL 中没有三角形");
        return false;
    }
    if (count > quint32(kMaxTriangles)) {
        *error = QStringLiteral("三角形数量 %1 超过上限 %2，请先简化模型")
                     .arg(count)
                     .arg(kMaxTriangles);
        return false;
    }

    mesh->triangles.reserve(int(count));
    const uchar *p = base + 84;
    for (quint32 i = 0; i < count; ++i, p += 50) {
        const QVector3D a(readFloat(p + 12), readFloat(p + 16), readFloat(p + 20));
        const QVector3D b(readFloat(p + 24), readFloat(p + 28), readFloat(p + 32));
        const QVector3D c(readFloat(p + 36), readFloat(p + 40), readFloat(p + 44));
        accumulate(mesh, a, b, c);
    }
    return true;
}

bool parseAscii(const QByteArray &data, StlMesh *mesh, QString *error)
{
    const char *p   = data.constData();
    const char *end = p + data.size();

    QVector3D verts[3];
    int filled = 0;

    while (p < end) {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'))
            ++p;
        if (p >= end)
            break;

        const char *tokenStart = p;
        while (p < end && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n')
            ++p;

        const int tokenLen = int(p - tokenStart);
        if (tokenLen != 6 || qstrnicmp(tokenStart, "vertex", 6) != 0)
            continue;

        double xyz[3] = {0.0, 0.0, 0.0};
        bool allOk = true;
        for (double &value : xyz) {
            bool ok = false;
            value = readDouble(&p, end, &ok);
            allOk = allOk && ok;
        }
        if (!allOk) {
            *error = QStringLiteral("ASCII STL 的 vertex 行格式不正确");
            return false;
        }

        verts[filled++] = QVector3D(float(xyz[0]), float(xyz[1]), float(xyz[2]));
        if (filled == 3) {
            accumulate(mesh, verts[0], verts[1], verts[2]);
            filled = 0;
            if (mesh->triangles.size() > kMaxTriangles) {
                *error = QStringLiteral("三角形数量超过上限 %1，请先简化模型").arg(kMaxTriangles);
                return false;
            }
        }
    }

    if (mesh->triangles.isEmpty()) {
        *error = QStringLiteral("STL 中没有可解析的三角形");
        return false;
    }
    return true;
}

} // namespace

double StlMesh::maxExtent() const
{
    const QVector3D s = size();
    return qMax(s.x(), qMax(s.y(), s.z()));
}

StlLoadResult loadStl(const QString &path)
{
    StlLoadResult result;

    QFileInfo info(path);
    if (!info.exists() || !info.isFile()) {
        result.error = QStringLiteral("文件不存在：%1").arg(path);
        return result;
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        result.error = QStringLiteral("无法打开文件：%1").arg(file.errorString());
        return result;
    }

    const QByteArray data = file.readAll();
    file.close();

    if (data.size() < 84) {
        result.error = QStringLiteral("文件过小，不是有效的 STL（%1 字节）").arg(data.size());
        return result;
    }

    StlMesh mesh;
    mesh.sourcePath = info.absoluteFilePath();

    // 二进制 STL 的长度是刚性的：84 + 50×n。先按长度判定，避免把
    // 以 "solid" 开头的二进制文件误当 ASCII 解析。
    const quint32 headerCount = qFromLittleEndian<quint32>(
        reinterpret_cast<const uchar *>(data.constData()) + 80);
    const bool looksBinary = (qint64(data.size()) == 84 + qint64(headerCount) * 50);

    QString error;
    const bool ok = looksBinary ? parseBinary(data, &mesh, &error)
                                : parseAscii(data, &mesh, &error);
    if (!ok) {
        result.error = error.isEmpty() ? QStringLiteral("无法识别的 STL 格式") : error;
        return result;
    }

    // 有向体积求和依赖绕序；反向绕序的模型会得到负值，取绝对值即可
    mesh.volume = qAbs(mesh.volume);

    result.ok     = true;
    result.format = looksBinary ? StlLoadResult::Binary : StlLoadResult::Ascii;
    result.mesh   = mesh;
    return result;
}

QString stlSummary(const StlMesh &mesh)
{
    if (mesh.isEmpty())
        return QStringLiteral("未载入几何");

    const QVector3D s = mesh.size();
    return QStringLiteral("%1 面 · %2×%3×%4 mm · %5 cm³")
        .arg(QLocale().toString(mesh.count()),
             QString::number(s.x(), 'f', 1),
             QString::number(s.y(), 'f', 1),
             QString::number(s.z(), 'f', 1),
             QString::number(mesh.volume / 1000.0, 'f', 2));
}
