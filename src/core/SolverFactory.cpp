#include "SolverFactory.h"
#include "MockSolverBackend.h"

#ifdef QTG_WITH_CLOSED_SOLVER
// 仅存在于 src/private/ 的闭源封装层，不随仓库上传。
// 该头文件负责把加密静态库的 C ABI 包装成 ISolverBackend。
#include "ClosedSolverBackend.h"
#endif

ISolverBackend *createSolverBackend()
{
#ifdef QTG_WITH_CLOSED_SOLVER
    return new ClosedSolverBackend();
#else
    return new MockSolverBackend();
#endif
}
