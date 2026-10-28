#pragma once

#include "ISolverBackend.h"

/**
 * 求解后端工厂。
 *
 * 编译期由 QTG_USE_CLOSED_SOLVER 决定返回闭源实现还是内置示意实现，
 * 调用方（界面层）只拿到 ISolverBackend*，不感知差异。
 *
 * 返回对象所有权交给调用方（建议用 std::unique_ptr 持有）。
 */
ISolverBackend *createSolverBackend();
