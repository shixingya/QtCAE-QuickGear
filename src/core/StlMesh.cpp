#include "StlMesh.h"

#include <QByteArray>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QLocale>
#include <QtEndian>
#include <QtMath>

#include <cmath>
#include <cstring>

namespace {

/** 单文件上限：再大就不是"轻量化"工具该吃的模型了，直接拒绝比崩溃体面 */
constexpr int kMaxTriangles = 2000000;

/**
 * 封闭性统计的上限。一遍焊接 + 边配对是 O(n) 但每面 3 次哈希，
 * 200 万面的哈希表要塞几百 MB；超过就报"没统计"，不拖死界面。
 */
constexpr int kSolidityMaxTriangles = 200000;

struct VertexKey
{
    qint64 x = 0;
    qint64 y = 0;
    qint64 z = 0;

    bool operator==(const VertexKey &other) const
    {
        return x == other.x && y == other.y && z == other.z;
    }
};

inline uint qHash(const VertexKey &key, uint seed = 0)
{
    return ::qHash(key.x, seed) ^ (::qHash(key.y, seed) << 1) ^ (::qHash(key.z, seed) << 2);
}

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

/**
 * 封闭性 + 绕序一致性统计。
 *
 * STL 不共享顶点，所以得先按坐标焊接。量子步长取包围盒最大边长的百万分之一：
 * 100 mm 的件约 0.1 μm，远小于任何可加工特征，又远大于 float32 在该量级上的
 * 舍入噪声（≈0.008 μm），不会把本该重合的顶点拆成两个。
 *
 * 封闭与绕序一致是两件事，而它们只需要同一遍边配对就能同时得到：
 * 一致的封闭网格里，每条无向边必须正好被两个三角形以相反方向共用一次。
 */
void analyzeSolidity(StlMesh *mesh)
{
    mesh->solidity = MeshSolidity::Unknown;
    const int count = mesh->triangles.size();
    if (count <= 0 || count > kSolidityMaxTriangles)
        return;

    const double eps = qMax(double(mesh->maxExtent()) * 1e-6, 1e-9);

    QHash<VertexKey, quint32> ids;
    ids.reserve(count * 2);
    const auto idOf = [&ids, eps](const QVector3D &p) -> quint32 {
        const VertexKey key{ qint64(qRound64(double(p.x()) / eps)),
                             qint64(qRound64(double(p.y()) / eps)),
                             qint64(qRound64(double(p.z()) / eps)) };
        const auto it = ids.constFind(key);
        if (it != ids.constEnd())
            return it.value();
        const quint32 id = quint32(ids.size());
        ids.insert(key, id);
        return id;
    };

    // 每条无向边打包两个方向的计数：低 8 位正向，高 8 位反向
    QHash<quint64, quint32> edges;
    edges.reserve(count * 3);

    for (const StlTriangle &tri : mesh->triangles) {
        const quint32 v[3] = { idOf(tri.a), idOf(tri.b), idOf(tri.c) };
        for (int i = 0; i < 3; ++i) {
            const quint32 from = v[i];
            const quint32 to   = v[(i + 1) % 3];
            if (from == to)
                continue;   // 退化成点的边不参与配对，否则整块会被误判成开口
            const quint64 lo = qMin<quint64>(from, to);
            const quint64 hi = qMax<quint64>(from, to);
            quint32 &packed = edges[(lo << 32) | hi];
            packed += (from < to) ? 1u : (1u << 8);
        }
    }

    bool open       = false;
    bool repeated   = false;
    for (quint32 packed : edges) {
        const quint32 forward = packed & 0xFFu;
        const quint32 reverse = (packed >> 8) & 0xFFu;
        if (forward == 0 || reverse == 0) {
            open = true;            // 有边只被一个面用：曲面片、破洞、未缝合
        } else if (forward != 1 || reverse != 1) {
            repeated = true;        // 同向重复：绕序内翻，或一条边被 4 个面共用
        }
    }

    mesh->solidity = open ? MeshSolidity::Open
                          : (repeated ? MeshSolidity::Inconsistent : MeshSolidity::Closed);
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
    // 不够 50×n 就没得读。调用方已经拦过，这里只防将来多一个入口时越界
    if (qint64(data.size()) < 84 + qint64(count) * 50) {
        *error = QStringLiteral("二进制 STL 被截断：文件头记录 %1 个三角形，需要 %2 字节，实际只有 %3 字节")
                     .arg(count)
                     .arg(84 + qint64(count) * 50)
                     .arg(data.size());
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
    int filled    = 0;
    int malformed = 0;   // 顶点数不足 3 的 facet

    while (p < end) {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'))
            ++p;
        if (p >= end)
            break;

        const char *tokenStart = p;
        while (p < end && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n')
            ++p;

        const int tokenLen = int(p - tokenStart);

        // endfacet 是 facet 的边界。没凑够 3 个顶点就得整块丢掉：
        // 否则上一块的残余顶点会和下一块的顶点拼出一个根本不存在的三角形，
        // 而那种三角形面积、体积、包围盒全是错的，用户无从发现。
        if (tokenLen == 8 && qstrnicmp(tokenStart, "endfacet", 8) == 0) {
            if (filled > 0) {
                ++malformed;
                filled = 0;
            }
            continue;
        }

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

        // 一个 facet 里超过 3 个顶点也是坏文件：只取前 3 个，剩下的在
        // endfacet 处连整块一起记一笔
        if (filled < 3)
            verts[filled] = QVector3D(float(xyz[0]), float(xyz[1]), float(xyz[2]));
        if (++filled == 3) {
            accumulate(mesh, verts[0], verts[1], verts[2]);
            filled = 0;
            if (mesh->triangles.size() > kMaxTriangles) {
                *error = QStringLiteral("三角形数量超过上限 %1，请先简化模型").arg(kMaxTriangles);
                return false;
            }
        }
    }

    if (filled > 0)
        ++malformed;   // 文件末尾没有 endfacet 的残块

    mesh->malformedFacets = malformed;

    if (mesh->triangles.isEmpty()) {
        *error = malformed > 0
                     ? QStringLiteral("STL 中没有可解析的三角形（%1 个 facet 顶点数不足 3）")
                           .arg(malformed)
                     : QStringLiteral("STL 中没有可解析的三角形");
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

    // 二进制 STL 的长度是刚性的：84 + 50×n。
    // 为什么不只看"是否以 solid 开头"：二进制文件也可能以 solid 开头。
    // 为什么不要求长度精确相等：真实文件常带尾部填充（换行、对齐零字节），
    // 一律当 ASCII 去解析只会得到"没有可解析的三角形"这种误导人的结论。
    // 反向误判也很安全：ASCII 文本的字节 80..84 不可能凑出一个小整数，
    // 算出来的刚性长度只会大到不合理，自然落回 ASCII 分支。
    const quint32 headerCount = qFromLittleEndian<quint32>(
        reinterpret_cast<const uchar *>(data.constData()) + 80);
    const qint64 binaryBytes = 84 + qint64(headerCount) * 50;
    const qint64 actualBytes = qint64(data.size());

    bool looksBinary = headerCount > 0 && actualBytes >= binaryBytes;

    if (headerCount > 0 && actualBytes < binaryBytes && !data.contains("vertex")) {
        // 头里写着 n 个三角形、实际字节不够，且通篇没有 vertex：被截断的二进制。
        // 直接说清楚，别让用户看到"没有可解析的三角形"去怀疑自己的模型。
        result.error =
            QStringLiteral("二进制 STL 被截断：文件头记录 %1 个三角形（需 %2 字节），实际只有 %3 字节")
                .arg(headerCount)
                .arg(binaryBytes)
                .arg(actualBytes);
        return result;
    }

    QString error;
    const bool ok = looksBinary ? parseBinary(data, &mesh, &error)
                                : parseAscii(data, &mesh, &error);
    if (!ok) {
        result.error = error.isEmpty() ? QStringLiteral("无法识别的 STL 格式") : error;
        return result;
    }

    // 有向体积求和依赖绕序；反向绕序的模型会得到负值，取绝对值即可
    mesh.volume = qAbs(mesh.volume);
    analyzeSolidity(&mesh);

    result.ok     = true;
    result.format = looksBinary ? StlLoadResult::Binary : StlLoadResult::Ascii;
    result.mesh   = mesh;
    return result;
}

QString solidityName(MeshSolidity solidity)
{
    switch (solidity) {
    case MeshSolidity::Closed:
        return QStringLiteral("封闭");
    case MeshSolidity::Open:
        return QStringLiteral("非封闭");
    case MeshSolidity::Inconsistent:
        return QStringLiteral("绕序不一致");
    case MeshSolidity::Unknown:
        break;
    }
    return QStringLiteral("未统计");
}

QString stlSummary(const StlMesh &mesh)
{
    if (mesh.isEmpty())
        return QStringLiteral("未载入几何");

    const QVector3D s = mesh.size();
    QString summary = QStringLiteral("%1 面 · %2×%3×%4 mm")
                          .arg(QLocale().toString(mesh.count()),
                               QString::number(s.x(), 'f', 1),
                               QString::number(s.y(), 'f', 1),
                               QString::number(s.z(), 'f', 1));

    // 体积只在封闭时才是真值：开口曲面的有向体积求和什么都不是，
    // 报一个带小数点的数上去，比不报更糟
    switch (mesh.solidity) {
    case MeshSolidity::Closed:
        summary += QStringLiteral(" · %1 cm³").arg(QString::number(mesh.volume / 1000.0, 'f', 2));
        break;
    case MeshSolidity::Open:
        summary += QStringLiteral(" · 体积不适用（网格非封闭）");
        break;
    case MeshSolidity::Inconsistent:
        summary += QStringLiteral(" · 体积不适用（绕序不一致）");
        break;
    case MeshSolidity::Unknown:
        summary += QStringLiteral(" · 体积未统计（面数超出统计上限）");
        break;
    }

    if (mesh.malformedFacets > 0)
        summary += QStringLiteral(" · 跳过 %1 个残缺面").arg(mesh.malformedFacets);

    return summary;
}
