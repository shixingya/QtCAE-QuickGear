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
│  ├─ MainWindow.*             主窗口：菜单 / 工程文件 / 拖放 / 日志 / 导出门控
│  ├─ ParamPanel.*             参数面板：齿数、模数、扭矩、齿宽、材料 + 工程性提醒
│  ├─ StressView.*             云图视图：按齿着色 + 色标；未求解时画导入几何的投影预览
│  ├─ ReportBuilder.*          结论页：单页 A4 PDF / 云图 PNG + 试用水印
│  ├─ SolveWorker.*            求解工作对象（跑在工作线程）
│  └─ ActivationDialog.*       激活对话框：机器码 / 授权状态 / 导入授权
├─ core/  （编译为 qgcore 静态库，界面与工具共用）
│  ├─ GearParams.h             输入参数（单位：mm / N·m / MPa）
│  ├─ ISolverBackend.h         求解后端抽象 + SolveResult
│  ├─ MockSolverBackend.*      内置示意后端（经验公式，非有限元）
│  ├─ SolverFactory.*          按编译开关选择后端
│  ├─ StlMesh.*                STL 读取（二进制 / ASCII）、包围盒、体积、表面积
│  ├─ ProjectFile.*            .qgpj 工程文件（参数 + 结果摘要 + 几何路径 + 备注）
│  └─ LicenseManager.*         机器码生成 + 授权文件签名/验签
└─ tools/
   ├─ license_issuer/          qglicense：授权签发与校验（控制台，开发者自用）
   └─ selftest/                qgselftest：不点鼠标的冒烟自检（见下文）
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
| `build/qgselftest.exe` | 冒烟自检，控制台程序，**不要随安装包发行** |

运行前确保 Qt 的 `bin` 目录在 `PATH` 中，或先部署依赖（见下文「免安装包」）。

Qt 6 亦可：`-DCMAKE_PREFIX_PATH=D:\Qt\6.x.x\msvc2019_64`，CMake 里 `find_package(QT NAMES Qt6 Qt5)` 会自动适配。

---

## 一件活的闭环：几何 → 求解 → 结论页

界面不再只是“点一下出张图”，而是能交付一个文件：

```text
导入 STL（.stl，二进制 / ASCII 自动识别）
   ↓  预览态：视口画几何的等轴测投影，标一行“xx 面 · 尺寸 · 仅预览，不参与求解”
运行仿真（Ctrl+R）
   ↓  视口切云图 + 色标；结果同时进工程
导出结论 PDF（Ctrl+E）/ 云图 PNG（Ctrl+Shift+E）
   ↓  A4 单页：结论句 + 输入表 + 结果表 + 云图 + 工况备注 + 免责声明
保存工程（Ctrl+S）→ .qgpj
   ↓  下次打开连结果一起回来，不用重算
```

| 操作 | 入口 | 说明 |
| --- | --- | --- |
| 新建 / 打开 / 最近打开 / 保存 / 另存为 | 文件菜单 | 关闭或切换工程时会问“未保存的修改” |
| 导入 STL | 文件菜单 / `Ctrl+I` / 拖文件进来 | `.qgpj` 也能直接拖进来打开 |
| 运行仿真 | 按钮 / `Ctrl+R` | 求解期间参数面板锁定，进度条与日志同步 |
| 导出结论 PDF | `Ctrl+E` | 单页 A4、150 dpi；未授权时云图打“未授权试用 · 示意数据”水印 |
| 导出云图 PNG | `Ctrl+Shift+E` | 离屏 760×600 渲染，不是截屏 |
| 求解日志 | 视图 → 求解日志 | 停靠面板，带时间戳，上限 2000 行 |
| 参数 / 窗口 / 最近文件 | 自动 | 存在 `QSettings`，下次启动还原 |

几条刻意的取舍：

- **导入几何只做预览，不参与求解。** 闭源求解器吃的是参数化齿轮，STL 只用来核对外形；
  视口会明确标“仅预览，不参与求解”，不出结果时也不挂色标——不画不存在的数据。
- **导出的图不是截屏。** 导出走离屏 `StressView` 单独渲染，PDF 里图例和标注不会糊。
- **结论页必须一页。** 自检里有硬断言（读 PDF 对象的 `/Count`），排版漂移会当场失败。
  实测云图宽 680 px 是单页上限，760 px 就会溢出成两页。
  另外 `QTextDocument::print()` 会按满页重排再缩放，比按内边距排出来高一大截，
  所以 `ReportBuilder` 自己是翻页的（`setPageSize` + `drawContents` 裁剪）。
- **未授权不等于不能导出**，但云图和 PDF 都会打水印，报告里写明“未授权 · 试用版导出”。

工程文件 `.qgpj` 是 JSON（`formatVersion: 2`），存参数、结果摘要（含每齿应力数组）、几何路径、
工况备注与保存时间，用 `QSaveFile` 原子写——写到一半断电也不会留下半个文件。
几何只存路径：文件被移走后打开工程会降级为“无几何”并在日志里说清原因，不假装还在。

---

## 冒烟自检（qgselftest）

改完代码先跑这个，比手动点界面快，也比肉眼看图靠谱：

```powershell
.\build\qgselftest.exe --out out\selftest
```

退出码 `0` 表示全过。当前 20 项，覆盖：

- **STL**：自己写 ASCII / 二进制两份再读回来，逐项比对面数、包围盒、体积（两格式体积差 < 1 mm³）
- **求解**：后端能创建、能出非零应力与每齿数组
- **渲染**：视口非空 + **饱和像素占比**断言（当初就是它抓到“视口空白”的绘制顺序 bug）
- **导出**：PDF 大小、**页数必须为 1**、PNG 大小、水印确实叠上了
- **结论页**：用同一份 HTML 渲染一张 `report_preview.png`，汉字错位/豆腐块一眼能看出来
- **工程文件**：存了再读，逐字段比对（含备注、几何路径、每齿数组长度）
- **授权**：签名能过、改一个字节就不过

产物都在 `--out` 目录：`report.pdf`、`report_preview.png`、`stress.png`、`mesh_preview.png`、`selftest.qgpj`。

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
- [x] STL 导入（二进制 / ASCII）+ 未求解时的几何预览
- [x] 结论页导出（单页 A4 PDF）/ 云图导出（PNG），未授权自动加水印
- [x] 工程文件 `.qgpj`（参数 + 结果 + 几何路径 + 备注，原子写）
- [x] 拖放打开、最近文件、`QSettings` 记忆、求解日志面板
- [x] `qgselftest` 冒烟自检（20 项，含 PDF 页数与渲染覆盖率断言）
- [ ] 闭源求解器接入（需加密库）
- [ ] 签名升级为 Ed25519 公钥验签（共享密钥可被逆向）
- [ ] 三维云图渲染（OpenGL / VTK）
- [ ] 导入几何参与求解（网格修复 / 实体化，目前仅预览）
- [ ] 批量参数扫描 / 报告模板自定义
