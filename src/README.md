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
│  ├─ StlMesh.*                STL 读取（二进制 / ASCII）、包围盒、体积、表面积、
│  │                           封闭性与绕序一致性
│  ├─ ProjectFile.*            .qgpj 工程文件（参数 + 结果摘要 + 几何路径 + 备注）
│  ├─ LicenseManager.*         机器码生成 + 授权签名/验签 + 状态与剩余天数
│  └─ LicenseBatch.*           台账批量签发 + 交付单（manifest.csv）
└─ tools/
   ├─ license_issuer/          qglicense：签发 / 批量签发 / 到期审计（控制台，开发者自用）
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
- **体积只在网格封闭时才敢报。** 读入后做一遍顶点焊接 + 边配对：每条无向边必须被两个
  三角形以相反方向各用一次，才算封闭且绕序一致。开口曲面片算出来的有向体积是个无意义的数，
  界面写“体积不适用（网格非封闭）”，而不是给一个带小数点的假数。面数超过 20 万就不做这项
  统计（哈希表会吃几百 MB），报“未统计”。
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

退出码 `0` 表示全过。当前 48 项，覆盖：

- **STL**：自己写 ASCII / 二进制两份再读回来，逐项比对面数、包围盒、体积（两格式体积差 < 1 mm³）；
  体积还要对得上解析值（星形柱体 74.540 cm³，偏差 < 1%）——只断言 `volume > 0` 是拦不住错的，
  开口曲面的有向体积也可能是正数
- **STL 容错**：尾部补 9 个字节仍按二进制读（不能因为长度不精确就回落到 ASCII）；
  截断文件报“需 N 字节、实际只有 M 字节”；一个 facet 只有 2 个顶点时整块丢弃并计数
  （否则上一块的残顶点会和下一块拼出一个不存在的三角形）；抽掉一块面后必须判为“非封闭”
  且体积不可报
- **求解**：后端能创建、能出非零应力与每齿数组
- **口径对齐**：直接读 `docs/index.html` 的常量与客户端对账（量程 320 / 许用 400 / 截断 ×2 / 取整规则），
  改了一边没改另一边，自检会报
- **渲染**：视口非空 + **饱和像素占比** 断言（当初就是它抓到“视口空白”的绘制顺序 bug）
- **导出**：PDF 大小、**页数必须为 1**、PNG 大小、水印确实叠上了
- **结论页**：用同一份 HTML 渲染一张 `report_preview.png`，汉字错位/豆腐块一眼能看出来
- **工程文件**：存了再读，逐字段比对（含备注、几何路径、每齿数组长度）
- **授权**：签名能过、改一个字节就不过；机器码连写小写能被归一化；
  剩 10 天判 `Valid` 且在提醒窗口内、过期 3 天判 `Expired`（过期必须盖过“机器匹配”）；
  台账 5 行里 1 行坏码只拒它自己；批量写出 2 份 + 交付单 2 行且带 BOM；
  **dry-run 不许建目录、不许落盘**
- **发版物**：读 `QuickGear.exe` 的版本资源，`FileVersion` / `ProductVersion` 必须等于
  `project(... VERSION ...)`。版本号一度散在三处（工程、自检页脚、docs），
  现在只剩一处来源，exe 属性里能看到什么就是什么

产物都在 `--out` 目录：`report.pdf`、`report_preview.png`、`stress.png`、`mesh_preview.png`、
`selftest.qgpj`，以及 `issued/`（批量签发的授权与 `manifest.csv`）。

### 网页端也要体检：`docs/webcheck.js`

客户端改了公式，网页那边不会报错——它只会默默给出另一个数。发布前从仓库根目录跑：

```bash
node docs/webcheck.js
```

它把 `docs/index.html` 的内联脚本放进 node 沙箱真跑一遍，验证：JS 语法与顶层执行没问题、
`estimateResults()` 与 `MockSolverBackend::solve` 在 5 组参数上逐字段一致（σ / 位移 / 单元数 / n）、
降级指纹仍产出合法 32 位 hex。不需要开浏览器，也不需要网络。

---

## 免安装包（发给试用用户）

发版时从仓库根目录跑一句（PATH 里需要有 Qt 的 `bin` 和 MinGW 的 `bin`，脚本靠 `qmake -query` 自己找 DLL）：

```powershell
powershell -ExecutionPolicy Bypass -File tools\pack.ps1
```

它会做三件手搓容易漏的事：核对 `QuickGear.exe` 的版本资源跟 `project(VERSION)` 是否一致（对不上直接拒绝打包，免得把上一版的壳发给客户）；按固定清单拉齐 9 个二进制；再写一份 `使用说明.txt`（运行、授权、MOCK 角标、怎么报问题）一起压成 zip。

`windeployqt` 在部分 Qt 安装下会报 `Unable to find the platform plugin`，所以脚本直接按清单取源文件，不走它：

```text
dist/QuickGear-0.4.3-win64/
├─ QuickGear.exe
├─ Qt5Core.dll  Qt5Gui.dll  Qt5Widgets.dll
├─ platforms/qwindows.dll        ← 缺这个会报 “could not find or load the Qt platform plugin”
├─ styles/qwindowsvistastyle.dll
├─ libgcc_s_seh-1.dll  libstdc++-6.dll  libwinpthread-1.dll   ← MinGW 运行时
└─ 使用说明.txt                   ← 试用用户手上唯一的一份说明
```

实测体积：解包 30.3 MB，zip 11.1 MB。`dist/` 已被 `.gitignore` 屏蔽，走 Release 附件或网盘分发。

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
| 显示位置 | 菜单「授权 → 激活 / 查看机器码…」；状态栏徽标见下表 |
| 授权文件 | `%APPDATA%/shixingya/QtCAE-QuickGear/license.qglic`（JSON） |
| 校验顺序 | 结构 → 签名 → 机器码绑定 → 有效期，任一项不过即判无效并给出具体原因 |
| 签名算法 | 默认 **Ed25519 公钥签名**（客户端只带公钥，拆开也签不出授权）；旧文件无 `alg` 字段时按 HMAC-SHA256 兼容读取 |
| 签名串 | `machine\|licensee\|edition\|issued\|expire`，签发与验签共用 `LicenseManager::canonicalString()`，常时间比较 |
| 机器码归一化 | 大写、去空格、缺连字符的 32 位十六进制自动补分组——客户手抄漏横杠也能签 |

> 为什么先验签再比机器码：一张被改过的文件不该以“机器不匹配”的名义回复客户，
> 那会让双方都以为是对方的机器出了问题。

### 密钥对：生成一次，只把公钥交出去

授权强度全靠这一步。私钥留在你的签发机上，公钥编进客户端——**客户端被拆开也签不出新授权**。

```powershell
# 1. 生成密钥对（随机种子，不联网）
.\build\qglicense.exe keygen --out D:\keys\quickgear.key
#    FILE=D:\keys\quickgear.key
#    SEED=RaxAa+3lIuneFceb52ONsJUdtEGyVkVMm0Cm845KEgU=      ← 私钥，别进仓库、别进网盘
#    PUBLIC=6N5/Rcqrj3OoPt7JYiE9t5hf0pgmYv2kPIx0uHwo/ek=    ← 公钥，可以公开

# 2. 构建客户端：只注入公钥
cmake -S . -B build -DQTG_LICENSE_PUBKEY="<上一步的 PUBLIC>"

# 3. 签发：私钥用文件传入（也可以用环境变量 QTG_SIGNING_KEY）
.\build\qglicense.exe issue --licensee "某某机械厂" --days 365 --key D:\keys\quickgear.key
```

- 私钥文件支持 base64 / 64 位 hex / 原始 32 字节三种写法，`seedFromText()` 都能认。
- 换机器重建客户端时，不用翻旧提交找公钥：
  `qglicense pubkey --key <私钥>` 直接输出 `PUBLIC=` 与可粘贴的 `CMAKE=-DQTG_LICENSE_PUBKEY=...`，
  同时告诉你它跟当前构建内置的公钥是 `MATCH` 还是 `DIFFER`。
- 仓库里 `QTG_LICENSE_PUBKEY` / `QTG_SIGNING_SEED` 的默认值是**公开的演示对**，
  只为让新克隆能开箱跑通“签发 → 安装 → 验签”。真发版必须换成你自己 `keygen` 出来的那套。
- `qglicense issue` 签完会自动比对：这把私钥派生出的公钥和当前构建内置的公钥不一致时
  输出 `WARN=signing key does not match...` —— 这一句能省掉“客户装上去说签名无效”的来回。
- `keygen` 输出里的 `EMBEDDED=MATCH/DIFFER` 是同一道防呆：拿错私钥当场就能看出来。

### 状态栏徽标与到期提醒

| 情形 | 徽标 | 颜色 |
| --- | --- | --- |
| 正常（距到期 > 30 天） | `授权：已激活（pro）` | 绿 |
| 30 天内到期 | `授权：pro · 剩 12 天` | 橙 |
| 已过期 | `授权：已过期` | 红 |
| 绑的不是这台机器 | `授权：与本机不匹配` | 红 |
| 签名对不上 | `授权：文件无效` | 红 |
| 没装 / 读不出 | `授权：未激活（试用版）` | 橙 |

启动弹窗只在 **剩 7 天内** 与 **已过期** 两种情况下出现，弹窗里带「复制机器码」按钮，
一步就能把码发给作者。剩 30 天那种只把徽标变橙——天天弹窗只会让人学会无脑点掉，
真到期那天反而不看了。

### 签发单张授权

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
| `--machine <CODE>` | 默认绑本机；给客户签发时填客户机器码（连写/小写均可） |
| `--edition pro` | 版本标识，默认 `pro` |
| `--out <file>` | 输出路径，默认当前目录 `license-<机器码前9位>-<到期日>.qglic` |

### 批量签发：台账 → 一批授权 + 一张交付单

一次给十几家开授权，或者把去年名单按新到期日整体续一年，就走这里：

```powershell
# 1. 写台账（Excel 另存为 CSV 即可，带不带 BOM 都认）
#    machine,licensee,edition,days,expire
#    3f1b92c47e10ab3566120d8c4f5e2299,某某机械厂,pro,365,
#    A20A-...,"某某传动, 二分厂",pro,,2027-06-30

# 2. 先试跑：只打印打算写哪些文件，不建目录、不落盘
.\build\qglicense.exe batch --in 客户台账.csv --out issued --dry-run

# 3. 真签
.\build\qglicense.exe batch --in 客户台账.csv --out issued
```

| 参数 | 说明 |
| --- | --- |
| `--in <csv>` | 台账；列顺序固定为 `machine,licensee,edition,days,expire`，`#` 开头为注释，表头可有可无 |
| `--out <dir>` | 产物目录，默认 `./issued` |
| `--dry-run` | 只算目标文件名 |

台账的几条硬规矩（都是会被客户真踩到的坑）：

- `days` 与 `expire` 二选一，两者都给时以 `expire` 为准；
- 机器码可以连写、小写、多空格；格式不对的在签发前就拦下——
  **宁可在你这里报错，也不要让客户装上去看到「与本机不匹配」**；
- 一行坏掉只影响那一行，其余照发，退出码 `4` 提醒你回台账看是哪行；
- 文件名从授权对象清洗而来（逗号、斜线自动换 `-`），同一批重名加 `-2` 不覆盖。

```text
issued/
├─ qglic-某某机械厂-2027-10-29.qglic
├─ qglic-某某传动-二分厂-2027-06-30.qglic
├─ qglic-某某模具厂-2026-12-13.qglic
└─ manifest.csv     ← 交付单：机器码/授权对象/签发日/到期日/文件名/状态/算法/签名前 8 位/错误
```

`manifest.csv` 是带 BOM 的 UTF-8，Excel 双击直接看、不乱码。
它和授权文件是**同一次调用里一起写的**——只发文件不记账，
三批之后就说不清谁买到哪天了。

### 续费：一条命令，不用手抄三个字段

```powershell
# 拿旧的授权文件直接续一年（默认续本机已装的那张）
.\build\qglicense.exe renew issued\qglic-某某机械厂-2027-10-29.qglic --days 365
#    RENEWED=...\license-A20A-7BF9-2028-10-28.qglic
#    MACHINE=A20A-7BF9-... LICENSEE=某某机械厂
#    PREVIOUS_EXPIRES=2027-10-29
#    STARTS=2027-10-29  EXPIRES=2028-10-29
```

机器码、授权对象、版本都从旧文件里搬，只改日期。两条规则：

- **起算日 = max(今天, 旧到期日)**。提前续费不白丢剩余天数（从旧到期日接着算），
  过期半年才续也不至于只拿到剩下半张（从今天算）。
- **只给验签通过的旧授权续费**（`STATE=VALID` / `EXPIRED` / `FOREIGN`）。
  读不出或签名对不上的直接 `EXIT=4`，**一个文件都不会写**。
  这不是洁癖：续费等于拿我们的私钥给文件里的授权对象签名，一张伪造件里
  想填什么就填什么，照单签下去就是替伪造者把假授权洗成真的。

### 到期审计：谁该续费了

```powershell
# 扫已签的授权，列出窗口内到期 / 已过期 / 文件对不上的
.\build\qglicense.exe audit issued --days 30 --csv 续费清单.csv
```

```text
FILE=issued\qglic-某某模具厂-2026-12-13.qglic
  MACHINE=7710-4C2E-... LICENSEE=某某模具厂 EDITION=pro
  EXPIRES=2026-12-13 DAYS=45 STATE=VALID ALG=ed25519 FLAG=EXPIRING
SUMMARY TOTAL=3 OK=2 EXPIRING=1 EXPIRED=0 BROKEN=0 LEGACY=0 WINDOW=60
```

- `audit` **跳过机器码绑定校验**：客户的机器码本来就不是你本机的，
  照常校验的话每张都是 `FOREIGN`，一个到期日都看不到。签名仍然照验，
  所以 `EXPIRED`（该续费）与 `BROKEN`（文件被改过 / 不是自己签的）能分开统计。
- `ALG` 是每张授权的签名算法；`LEGACY=n` 是**仍用 HMAC 签的数量**，
  包括已经过期、正在谈续费的那些——它们下次动手就该换新算法。
  这个数不为 0 时，把对应的老客户排进换签清单。
- `FLAG` 四态：`OK` / `EXPIRING`（窗口内）/ `EXPIRED` / `BROKEN`。
- 有 `EXPIRING` / `EXPIRED` / `BROKEN` 时退出码 `4`，可挂进计划任务当续费看板。
- `--csv` 那份续费清单同样带 BOM，可直接当催款工作表。

> 控制台代码页不是 UTF-8 时，中文公司名可能显示成乱码；`--csv` 那份不会。

退出码：`0` 成功，`1` 用法错误或 verify 不通过，`2` 参数错误，`3` 写盘失败，
`4` batch 有失败行 / audit 发现待处理项。输出是 ASCII 键值对
（`VALID=YES`、`STATE=EXPIRED`、`FLAG=EXPIRING`），可直接在脚本里 grep。

dry-run 输出用 `PLANNED=` 而不是 `ISSUED=`：脚本里 grep 了 `ISSUED` 就等于以为文件已经发出去了。

### 授权文件格式

v0.4 起写 `v: 2` 与 `alg: "ed25519"`，`sig` 是 64 字节 Ed25519 签名的 base64：

```json
{
  "v": 2,
  "alg": "ed25519",
  "machine": "A20A-7BF9-1FA0-0DB1-C5C8-302C-B3D8-A600",
  "licensee": "某某机械厂",
  "edition": "pro",
  "issued": "2026-10-29",
  "expire": "2027-10-29",
  "sig": "vAvaB2GVdYl2hWgsIxYvJ/OdoFAoYzVBC3iXZAzuVKRo8D0fpNJCTv1ZlTUehB7...",
  "note": "QtCAE-QuickGear offline license"
}
```

v0.3 及更早的文件没有 `alg` 字段，按 HMAC 读取，照旧能用：

```json
{
  "v": 1,
  "machine": "A20A-7BF9-1FA0-0DB1-C5C8-302C-B3D8-A600",
  "licensee": "某某机械厂",
  "edition": "pro",
  "issued": "2026-10-28",
  "expire": "2027-10-28",
  "sig": "GjK78vbQzuFsi6dYNcuDZCH2KHbVklrTh0YT06C/GZ0="
}
```

`expire` 留空表示**永久授权**，此时 `daysLeft` 固定为 `-1`、徽标不会进入到期提醒；
`audit` 里这类会显示 `EXPIRES=perpetual DAYS=-1`，不计入续费清单。

读到 `v` 比客户端支持的版本（当前 `LicenseManager::kFormatVersion = 2`）更大时，
客户端直接报「授权文件格式为 vN，本客户端只支持到 v2：请升级客户端后再导入」，
不会拿旧规则去猜——将来真改了签名串或字段含义，客户看到的应该是「去升级」
而不是令人一头雾水的「签名无效」。

### ⚠️ 关于签名强度（务必读完再商用）

**这层挡的是什么、挡不住什么**，先说清楚：

| | Ed25519（v0.4 默认） | HMAC-SHA256（旧） |
| --- | --- | --- |
| 客户端里放的是什么 | 公钥（32 字节） | 共享密钥（要能算签名就必须有） |
| 拿到客户端后能否自己签授权 | **不能**：公钥只能验，签不出 | **能**：抠出密钥就能签任意机器、任意年限 |
| 抗“改文件” | 能，改一个字节即 `BADSIG` | 能 |
| 抗“逆向改判断” | 不能。改掉 `verifyEd25519()` 的返回值就绕过了 | 不能 |

所以：

1. **签名只防伪造授权文件，不防逆向**。真正的商业防线是闭源求解器
   （`QTG_USE_CLOSED_SOLVER`）——没有那部分，壳子被改也不产生价值。
2. 仓库默认密钥对是**公开的演示值**，用默认值构建的客户端一样拦不住人。
   真发版必须 `keygen` 一套自己的，并只把 `PUBLIC=` 交给构建。
3. 私钥一旦泄露，**所有**该公钥签发的授权都失效（客户端只认这一个公钥）。
   换钥相当于换一代授权：老授权在新客户端上会被当成 `BADSIG`，需要重新签发。
   私钥备份放在离线介质，别跟着代码走，也别放进网盘同步目录。
4. 已经实测的防线：改任一字段（如 `licensee`）、换机器、过期、拿另一把私钥签，
   客户端都会拒绝并给出具体原因。
5. 实现是自己写的（`core/Ed25519.cpp`，5×51 位域元素），没有引入外部依赖。
   验收靠 **RFC 8032 §7.1 的三组官方向量**：seed → 公钥、签名逐字节一致、
   以及改消息/改签名/换公钥/截短签名都必须被拒。这三项每次 `qgselftest` 都跑。

### 从 v0.3 升级：老授权怎么办

升级后客户端**同时认两种算法**，不需要停机换签：

- 已发出的 HMAC 授权照旧可用，`verify` / `audit` 会标 `ALG=hmac`；
- 新签的走 Ed25519（`ALG=ed25519`），客户端用内置公钥验；
- `audit` 的 `LEGACY=n` 告诉你有多少张还是老算法。建议在客户下次续费时顺手换签，
  不要专门为换算法做一次停服动作；
- 换公钥之后，**所有**老 Ed25519 授权会失效（公钥变了）。同一代授权不要中途换钥。

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
- [x] 机器码生成、Ed25519 公钥签名（旧 HMAC 授权仍可验）、验签与导入
- [x] `qglicense` 签发工具（machine / keygen / pubkey / issue / batch / renew / audit / verify / install）
- [x] 免安装包部署（手工部署 9 个文件，干净 PATH 可启动）
- [x] STL 导入（二进制 / ASCII）+ 未求解时的几何预览
- [x] STL 容错与诚实报数：尾部填充 / 截断 / 残缺 facet 都给准确原因，
  封闭性检测决定体积能不能报
- [x] 结论页导出（单页 A4 PDF）/ 云图导出（PNG），未授权自动加水印
- [x] 工程文件 `.qgpj`（参数 + 结果 + 几何路径 + 备注，原子写）
- [x] 拖放打开、最近文件、`QSettings` 记忆、求解日志面板
- [x] `qgselftest` 冒烟自检（48 项，含 Ed25519 RFC 8032 向量、续费门禁、PDF 页数与渲染覆盖率断言）
- [x] Windows 版本资源：exe 属性里的版本号跟 `project(VERSION)` 同一个来源
- [ ] 闭源求解器接入（需加密库）
- [x] 签名升级为 Ed25519 公钥验签（私钥只在签发侧）
- [x] 导入路径原子替换（不再“先删再拷”，失败不会两头落空）
- [ ] 三维云图渲染（OpenGL / VTK）
- [ ] 导入几何参与求解（网格修复 / 实体化，目前仅预览）
- [ ] 批量参数扫描 / 报告模板自定义
