#pragma once

#include "core/ISolverBackend.h"
#include "core/StlMesh.h"

#include <QColor>
#include <QImage>
#include <QVector>
#include <QWidget>

/**
 * 应力云图视图（2D 示意）。
 *
 * 真实客户端这里是 OpenGL / VTK 的三维场渲染；壳工程用 QPainter 画一个
 * "按齿着色的齿轮盘 + 图例"，把数据链路（参数 → 求解 → 逐齿应力 → 上色）跑通，
 * 便于后续替换渲染后端时对齐接口。
 */
class StressView : public QWidget
{
    Q_OBJECT

public:
    explicit StressView(QWidget *parent = nullptr);

    /** 参数变化时同步齿轮外形（不改变已有结果的着色逻辑） */
    void setGear(int teeth, int pinionTeeth, double module);

    /** 展示结果：逐齿峰值应力 + 量程上限 */
    void setResult(const SolveResult &result);

    /** 清空结果，回到金属灰显示 */
    void clearResult();

    /** 是否处于"示意后端"状态（右上角显示 MOCK 标记） */
    void setDemoMode(bool demo);

    /**
     * 载入导入几何。未求解时视图切换为该几何的等轴测投影，
     * 求解后仍显示齿轮云图（结果优先），仅在角落标注几何摘要。
     */
    void setMesh(const StlMesh &mesh);
    void clearMesh();
    bool hasMesh() const;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    void drawGear(QPainter &painter, const QRectF &area);
    void drawMesh(QPainter &painter, const QRectF &area);
    void drawMeshCaption(QPainter &painter, const QRectF &area);
    void drawLegend(QPainter &painter, const QRectF &area);
    QColor rampColor(double t) const;

    int     m_teeth       = 24;
    int     m_pinionTeeth = 12;
    double  m_module      = 2.0;
    bool    m_solved      = false;
    bool    m_demo        = true;
    double  m_ceiling     = 320.0;
    QVector<double> m_toothStress;

    StlMesh m_mesh;
    QImage  m_meshCache;   // 投影结果缓存，只在尺寸或几何变化时重画
};
