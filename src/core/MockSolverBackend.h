#pragma once

#include "ISolverBackend.h"

/**
 * 内置示意求解器。
 *
 * ⚠️ 这不是有限元求解器：它只按工程经验公式给出量级合理、趋势正确的数字，
 *    用于在没有闭源库的情况下把界面、流程、报告链路跑通。
 *    界面会显式标注"示意数据"，避免被误当作真实结果。
 *
 * 趋势依据：
 *   σ ∝ T / (m³ · √z)      —— 扭矩越大、模数越小、齿数越少，齿根弯应力越高
 *   δ ∝ σ                  —— 位移与应力同向变化
 *   单元数 ∝ z^1.15 · (2/m)^1.6 —— 齿数越多、模数越小，网格规模越大
 */
class MockSolverBackend : public ISolverBackend
{
public:
    QString displayName() const override;
    bool isDemo() const override { return true; }

    SolveResult solve(const GearParams &params, const ProgressFn &onProgress) override;
};
