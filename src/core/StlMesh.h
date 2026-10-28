#pragma once

#include <QString>
#include <QVector>
#include <QVector3D>

/**
 * 一个 STL 三角形（顶点绕序按右手法则，法向朝外）。
 */
struct StlTriangle
{
    QVector3D normal;
    QVector3D a;
    QVector3D b;
    QVector3D c;
};

/**
 * 导入的网格几何。
 *
 * 只做解析与统计，不做几何修复：真实产品里这一层会被 AI 网格模块替换，
 * 壳工程保持"读得进、算得出统计量、画得出投影"的最低可用能力。
 */
struct StlMesh
{
    QVector<StlTriangle> triangles;
    QVector3D            minCorner;
    QVector3D            maxCorner;
    double               volume = 0.0;   // mm³，由有向四面体体积求和
    double               area   = 0.0;   // mm²，表面积合计
    QString              sourcePath;

    bool    isEmpty() const { return triangles.isEmpty(); }
    int     count() const { return triangles.size(); }
    QVector3D size() const { return maxCorner - minCorner; }
    double  maxExtent() const;
};

/** 读取结果：区分"格式不支持"与"文件读不了"，便于给用户准确提示 */
struct StlLoadResult
{
    enum Format { None = 0, Ascii = 1, Binary = 2 };

    bool     ok     = false;
    int      format = None;
    QString  error;
    StlMesh  mesh;
};

/** 解析 STL 文件（自动识别 ASCII / 二进制），三角形数上限 200 万 */
StlLoadResult loadStl(const QString &path);

/** 供界面显示的一行摘要，如 "12,486 面 · 48.0×48.0×20.0 mm · 18.2 cm³" */
QString stlSummary(const StlMesh &mesh);
