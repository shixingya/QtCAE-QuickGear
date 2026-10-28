# Qt 壳工程（src/）

QtCAE-QuickGear 客户端的**开源壳界面**：主窗口、参数面板、云图视图、求解流程与激活链路。
核心求解器与 AI 网格模块是加密静态库，不在此目录中。

> 关键设计：界面只依赖 `ISolverBackend` 一个接口，因此**没有闭源库也能完整编译运行**
> （自动使用内置示意后端），方便别人看懂产品形态、也方便你独立调试 UI。

---

## 目录

```text
src/
├─ CMakeLists.txt
├─ main.cpp                    入口：QSS 主题、元类型注册
├─ app/
│  ├─ MainWindow.*             主窗口：布局 / 菜单 / 状态栏 / 求解调度
│  ├─ ParamPanel.*             参数面板：齿数、模数、扭矩、齿宽、材料
│  ├─ StressView.*             云图视图：QPainter 按齿着色 + 图例
│  ├─ SolveWorker.*            求解工作对象（跑在工作线程）
│  └─ ActivationDialog.*       激活对话框：机器码 / 授权状态 / 导入授权
├─ core/  （编译为 qgcore 静态库，界面与工具共用）
│  ├─ GearParams.h             输入参数（单位：mm / N·m / MPa）
│  ├─ ISolverBackend.h         求解后端抽象 + SolveResult
│  ├─ MockSolverBackend.*      内置示意后端（经验公式，非有限元）
│  ├─ SolverFactory.*          按编译开关选择后端
│  └─ LicenseManager.*         机器码生成 + 授权文件签名/验签
└─ tools/
   └─ license_issuer/          qglicense：授权签发与校验（控制台，开发者自用）
```

---

## 构建

已验证环境：**Qt 5.15.2 (MinGW 8.1 64-bit) + CMake 3.21 + Ninja**。

```powershell
cd src
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release `
      -DCMAKE_PREFIX_PATH=D:\Qt\5.15.2\mingw81_64
cmake --build build
```

产物：

| 目标 | 说明 |
| --- | --- |
| `build/QuickGear.exe` | Qt 客户端（开源壳） |
| `build/qglicense.exe` | 授权签发 / 校验工具，控制台程序，**不要随安装包发行** |

运行前确保 Qt 的 `bin` 目录在 `PATH` 中，或先部署依赖（见下文「免安装包」）。

Qt 6 亦可：`-DCMAKE_PREFIX_PATH=D:\Qt\6.x.x\msvc2019_64`，CMake 里 `find_package(QT NAMES Qt6 Qt5)` 会自动适配。

---

## 免安装包（发给试用用户）

`windeployqt` 在部分 Qt 安装下会报 `Unable to find the platform plugin`，此时手工部署更稳（本仓库实测用这种方式）：

```text
dist/QuickGear-0.1.0-win64/
├─ QuickGear.exe
├─ Qt5Core.dll  Qt5Gui.dll  Qt5Widgets.dll
├─ platforms/qwindows.dll        ← 缺这个会报 “could not find or load the Qt platform plugin”
├─ styles/qwindowsvistastyle.dll
└─ libgcc_s_seh-1.dll  libstdc++-6.dll  libwinpthread-1.dll   ← MinGW 运行时
```

实测体积：解包 30.1 MB，zip 11.1 MB。`dist/` 已被 `.gitignore` 屏蔽，走 Release 附件或网盘分发。

> 验证方式：把 Qt / MinGW 从 `PATH` 里去掉再启动，能起来才说明包是自包含的。

---

## 接入闭源求解器

```powershell
cmake -S . -B build -G Ninja -DQTG_USE_CLOSED_SOLVER=ON
```

需要自备以下内容（均不随仓库上传，`.gitignore` 已屏蔽）：

```text
src/private/
├─ ClosedSolverBackend.h     # 把静态库打包成 ISolverBackend
└─ lib/quickgear_solver.lib  # 加密静态库
```

`SolverFactory.cpp` 里已留好 `#ifdef QTG_WITH_CLOSED_SOLVER` 分支，无需改动界面代码。

**Mock 后端说明**：`MockSolverBackend` 用的是与网页端一致的经验式
（σ ∝ T / (m³√z)），只保证**趋势与量级合理**，用于跑通流程。
界面上会显示「示意数据 · MOCK」角标，不能用于工程判断。

---

## 机器码与授权

| 项 | 说明 |
| --- | --- |
| 机器码 | `SHA-256(machineUniqueId + 系统信息 + 注册表 MachineGuid)` 取前 32 位十六进制，按 4 位分组显示 |
| 显示位置 | 菜单「授权 → 激活 / 查看机器码…」，状态栏显示 `授权：已激活（pro）` / `授权：未激活（试用）` |
| 授权文件 | `%APPDATA%/shixingya/QtCAE-QuickGear/license.qglic`（JSON） |
| 校验顺序 | 结构 → 机器码匹配 → 有效期 → 签名，任一项不过即判无效并给出具体原因 |
| 签名算法 | HMAC-SHA256，签名串 `machine\|licensee\|edition\|issued\|expire`，验签常时间比较 |

### 签发授权

用 `qglicense`（与客户端共用 `qgcore`，算法不会跑偏）：

```powershell
# 1. 拿到客户机器码（客户在客户端里点「授权 → 激活 / 查看机器码…」）
.\build\qglicense.exe machine

# 2. 给他签发一年
.\build\qglicense.exe issue --licensee "某某机械厂" --days 365

# 3. 发前先自己验一遍
.\build\qglicense.exe verify license-A1B2-C3D4-2027-10-28.qglic

# 4. 拿到文件后在客户端「导入授权文件」；也可以命令行装
.\build\qglicense.exe install license-xxx.qglic
```

| 参数 | 说明 |
| --- | --- |
| `--licensee <name>` | 必填，授权对象，会显示在激活对话框里 |
| `--days 365` | 从签发日起算有效期，与 `--expire` 二选一 |
| `--expire 2027-10-28` | 直接指定到期日 |
| `--issued 2026-10-28` | 默认今天；补开历史授权或测试过期时才用 |
| `--machine <CODE>` | 默认绑本机；给客户签发时填客户机器码 |
| `--edition pro` | 版本标识，默认 `pro` |
| `--out <file>` | 输出路径，默认当前目录 `license-<机器码前9位>-<到期日>.qglic` |

退出码：`0` 成功/有效，`1` 校验不通过，`2` 参数错误，`3` 文件读写失败。输出是 ASCII 键值对（`VALID=YES` 等），可直接在脚本里 grep。

### 授权文件格式

```json
{
  "v": 1,
  "machine": "A20A-7BF9-1FA0-0DB1-C5C8-302C-B3D8-A600",
  "licensee": "某某机械厂",
  "edition": "pro",
  "issued": "2026-10-28",
  "expire": "2027-10-28",
  "sig": "GjK78vbQzuFsi6dYNcuDZCH2KHbVklrTh0YT06C/GZ0=",
  "note": "QtCAE-QuickGear offline license"
}
```

### ⚠️ 关于签名强度（务必读完再商用）

- 密钥是**编译期注入的共享密钥**（`-DQTG_LICENSE_SECRET=...`），客户端与签发工具必须用同一个值。
- 仓库默认值是公开的演示密钥 `QUICKGEAR-OSS-DEMO-KEY-DO-NOT-USE-IN-PRODUCTION`，**用默认值构建的客户端等于没有授权保护**：任何人都能照着源码算出合法签名。
- 正式发版请务必：
  1. `cmake -DQTG_LICENSE_SECRET="你自己的长随机串"`，或改用环境变量注入到 CI；
  2. 更彻底的做法是把 `verifySignature()` 换成闭源模块里的 **Ed25519 公钥验签**（私钥只在签发侧，客户端只带公钥）——接口不变，其余流程不用改。
- 已经实测的防线：改任一字段（如 `licensee`）、换机器、过期，客户端都会拒绝并给出原因。它挡的是「随手改文件」，不是「专门逆向」。

---

**重要**：网页端设备码（浏览器指纹）和客户端机器码（硬件信息）是**两套独立的标识**，
浏览器无法获得客户端机器码。所以对外文案统一为：
网页设备码用于沟通定位，**激活以客户端机器码为准**。

---

## 当前完成度

- [x] 主窗口布局 / 菜单 / 状态栏
- [x] 参数面板（含分度圆、齿顶圆、传动比联动提示）
- [x] 求解流程（QThread + 进度条 + 结果回填）
- [x] 云图视图（按齿着色 + 色标图例 + MOCK 角标）
- [x] 机器码生成、授权文件签名（HMAC-SHA256）、验签与导入
- [x] `qglicense` 签发工具（machine / issue / verify / install）
- [x] 免安装包部署（手工部署 9 个文件，干净 PATH 可启动）
- [ ] 闭源求解器接入（需加密库）
- [ ] 签名升级为 Ed25519 公钥验签（共享密钥可被逆向）
- [ ] 三维云图渲染（OpenGL / VTK）
- [ ] STL 导入与几何修复
- [ ] 云图 PNG / 简报 PDF 导出
