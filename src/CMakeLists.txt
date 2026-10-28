cmake_minimum_required(VERSION 3.16)
project(QtCAE-QuickGear VERSION 0.3.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# 版本号只有一个出处：project()。客户端的窗口标题、自检产物、签发工具都读它，
# 避免“标题写 0.2、发出去的包其实是 0.3”这种事
add_compile_definitions(QTG_VERSION="${PROJECT_VERSION}")

# Qt6 优先，退化到 Qt5（本机为 Qt 5.15.2 / MinGW 8.1）
find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Core Gui Widgets)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core Gui Widgets)

# ---------------------------------------------------------------------------
# 闭源求解器开关：
#   OFF（默认）= 内置 Mock 后端，壳工程可独立编译运行，用于展示界面与流程
#   ON          = 链接 src/private/ 下的加密静态库（该目录不随仓库上传）
# ---------------------------------------------------------------------------
option(QTG_USE_CLOSED_SOLVER "链接闭源求解器静态库（需自备 src/private/）" OFF)

# ---------------------------------------------------------------------------
# 授权签名密钥（HMAC-SHA256 共享密钥）
#   ⚠️ 默认值是公开的演示密钥，任何人都能用来伪造授权。
#   正式发版请用 -DQTG_LICENSE_SECRET=... 换成自己的密钥，
#   且签发工具与客户端必须用同一个值；更彻底的做法见 src/README。
# ---------------------------------------------------------------------------
set(QTG_LICENSE_SECRET "QUICKGEAR-OSS-DEMO-KEY-DO-NOT-USE-IN-PRODUCTION" CACHE STRING "授权签名共享密钥")

# ---------------------------------------------------------------------------
# qgcore：与界面无关的核心库，被客户端与授权签发工具共用
# ---------------------------------------------------------------------------
add_library(qgcore STATIC
    core/GearParams.h
    core/ISolverBackend.h
    core/MockSolverBackend.h    core/MockSolverBackend.cpp
    core/SolverFactory.h        core/SolverFactory.cpp
    core/LicenseManager.h       core/LicenseManager.cpp
    core/LicenseBatch.h         core/LicenseBatch.cpp
    core/StlMesh.h              core/StlMesh.cpp
    core/ProjectFile.h          core/ProjectFile.cpp
)

target_include_directories(qgcore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# Gui 只为拿 QVector3D（几何解析用）；真正的求解器仍是纯计算库
# 不引入 QWidget，qglicense 才能作为控制台工具运行
target_link_libraries(qgcore PUBLIC Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui)
target_compile_definitions(qgcore PRIVATE QTG_LICENSE_SECRET="${QTG_LICENSE_SECRET}")

if (QTG_USE_CLOSED_SOLVER)
    target_compile_definitions(qgcore PUBLIC QTG_WITH_CLOSED_SOLVER=1)
    target_include_directories(qgcore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/private)
    find_library(QTG_SOLVER_LIB
        NAMES quickgear_solver quickgear_core
        HINTS ${CMAKE_CURRENT_SOURCE_DIR}/private/lib
    )
    if (NOT QTG_SOLVER_LIB)
        message(FATAL_ERROR "未找到闭源求解器库，请检查 src/private/lib 下的 .a/.lib 文件")
    endif()
    target_link_libraries(qgcore PRIVATE ${QTG_SOLVER_LIB})
    message(STATUS "已启用闭源求解器：${QTG_SOLVER_LIB}")
else()
    message(STATUS "闭源求解器未接入，使用 Mock 后端（结果为示意数据）")
endif()

# ---------------------------------------------------------------------------
# QuickGear：Qt 客户端（开源壳）
# ---------------------------------------------------------------------------
add_executable(QuickGear WIN32
    main.cpp
    app/MainWindow.h            app/MainWindow.cpp
    app/ParamPanel.h            app/ParamPanel.cpp
    app/StressView.h            app/StressView.cpp
    app/ActivationDialog.h      app/ActivationDialog.cpp
    app/SolveWorker.h           app/SolveWorker.cpp
    app/ReportBuilder.h         app/ReportBuilder.cpp
)

target_link_libraries(QuickGear PRIVATE qgcore Qt${QT_VERSION_MAJOR}::Widgets)

# ---------------------------------------------------------------------------
# qglicense：授权签发 / 校验工具（控制台，给开发者自己用）
# ---------------------------------------------------------------------------
add_executable(qglicense tools/license_issuer/main.cpp)
target_link_libraries(qglicense PRIVATE qgcore)

# ---------------------------------------------------------------------------
# qgselftest：不靠鼠标的冒烟自检（STL 解析 / 求解 / 导出 / 工程往返 / 验签）
#   qgselftest.exe --out <目录>   退出码 0 = 全过
# 复用客户端里两个与渲染相关的源文件，避免再造一份离线渲染代码
# ---------------------------------------------------------------------------
add_executable(qgselftest
    tools/selftest/main.cpp
    app/StressView.h            app/StressView.cpp
    app/ReportBuilder.h         app/ReportBuilder.cpp
)
target_include_directories(qgselftest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(qgselftest PRIVATE qgcore Qt${QT_VERSION_MAJOR}::Widgets)
