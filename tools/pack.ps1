<#
  免安装包打包（Windows x64）

    powershell -ExecutionPolicy Bypass -File tools\pack.ps1            # 版本号从 src/CMakeLists.txt 读
    powershell -ExecutionPolicy Bypass -File tools\pack.ps1 -SkipZip   # 只铺目录，不压 zip

  为什么要有这个脚本：这 10 个文件的清单一直是手抄的，少一个 dll 要等试用用户双击报
  “could not find or load the Qt platform plugin”才知道。这里把清单、版本校验、
  使用说明一次性钉死：exe 的版本资源跟工程版本对不上，直接拒绝打包。
#>
[CmdletBinding()]
param(
    [string] $Version  = "",
    [string] $BuildDir = "",
    [string] $QtBin    = "",
    [string] $QtPlugins = "",
    [string] $MingwBin = "",
    [switch] $SkipZip
)

$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
if (-not $BuildDir) { $BuildDir = Join-Path $root "src\build-cli" }

# ---- 1. 版本号：只认 project(... VERSION ...) 这一处 ----
if (-not $Version) {
    $cmakeFile = Join-Path $root "src\CMakeLists.txt"
    $cmake = Get-Content -LiteralPath $cmakeFile -Raw -Encoding UTF8
    if ($cmake -match 'project\s*\(\s*QtCAE-QuickGear\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)') {
        $Version = $Matches[1]
    } else {
        throw "读不出 src/CMakeLists.txt 里的 project(QtCAE-QuickGear VERSION x.y.z)"
    }
}
Write-Host "版本 $Version"

# ---- 2. 构建产物必须存在，且版本资源要跟工程版本一致 ----
$exe = Join-Path $BuildDir "QuickGear.exe"
if (-not (Test-Path -LiteralPath $exe)) {
    throw "找不到 $exe —— 先 cmake --build 再打包"
}
$exeVersion = (Get-Item -LiteralPath $exe).VersionInfo.FileVersion
if ($exeVersion -ne $Version) {
    throw "exe 版本资源是 '$exeVersion'，工程版本是 '$Version'。先重新 configure + build，别把上一版的壳发给客户"
}

# 清单以外的文件一律不进包：多带一个 dll 不会坏事，少带一个就一定起不来
# 三处来源：构建目录（exe）、Qt 的 bin 与 plugins、MinGW 运行时
if (-not $QtBin -or -not $QtPlugins) {
    $qmake = Get-Command qmake.exe -ErrorAction SilentlyContinue
    if (-not $qmake) {
        throw "PATH 里没有 qmake，定位不到 Qt 的 DLL：把 Qt 的 bin 加进 PATH，或用 -QtBin / -QtPlugins 指定"
    }
    if (-not $QtBin)     { $QtBin     = (& $qmake.Source -query QT_INSTALL_BINS).Trim() }
    if (-not $QtPlugins) { $QtPlugins = (& $qmake.Source -query QT_INSTALL_PLUGINS).Trim() }
}
if (-not $MingwBin) {
    $gpp = Get-Command g++.exe -ErrorAction SilentlyContinue
    if (-not $gpp) {
        throw "PATH 里没有 g++，定位不到 MinGW 运行时：把 MinGW 的 bin 加进 PATH，或用 -MingwBin 指定"
    }
    $MingwBin = Split-Path -Parent $gpp.Source
}

$entries = @(
    @{ Rel = "QuickGear.exe";              Dir = $BuildDir },
    @{ Rel = "Qt5Core.dll";                Dir = $QtBin },
    @{ Rel = "Qt5Gui.dll";                 Dir = $QtBin },
    @{ Rel = "Qt5Widgets.dll";             Dir = $QtBin },
    @{ Rel = "platforms\qwindows.dll";     Dir = $QtPlugins },
    @{ Rel = "styles\qwindowsvistastyle.dll"; Dir = $QtPlugins },
    @{ Rel = "libgcc_s_seh-1.dll";         Dir = $MingwBin },
    @{ Rel = "libstdc++-6.dll";            Dir = $MingwBin },
    @{ Rel = "libwinpthread-1.dll";        Dir = $MingwBin }
)

$missing = @()
foreach ($e in $entries) {
    if (-not (Test-Path -LiteralPath (Join-Path $e.Dir $e.Rel))) {
        $missing += ("{0}（在 {1}）" -f $e.Rel, $e.Dir)
    }
}
if ($missing.Count -gt 0) {
    throw ("包出去也起不来，这些找不到：`n  " + ($missing -join "`n  "))
}

# ---- 3. 铺目录 ----
$distRoot = Join-Path $root "dist"
$target   = Join-Path $distRoot ("QuickGear-{0}-win64" -f $Version)
if (Test-Path -LiteralPath $target) {
    Remove-Item -LiteralPath $target -Recurse -Force
}
New-Item -ItemType Directory -Path $target -Force | Out-Null

foreach ($e in $entries) {
    $dst    = Join-Path $target $e.Rel
    $dstDir = Split-Path -Parent $dst
    if (-not (Test-Path -LiteralPath $dstDir)) { New-Item -ItemType Directory -Path $dstDir -Force | Out-Null }
    Copy-Item -LiteralPath (Join-Path $e.Dir $e.Rel) -Destination $dst -Force
}

# ---- 4. 使用说明：试用用户拿到的只有这 10 个文件，没人替他解释 MOCK 角标 ----
$readme = @"
QtCAE-QuickGear $Version 免安装版（Windows x64）

一、运行
  双击 QuickGear.exe。不用安装，也不用管理员权限。
  本目录的 10 个文件必须放在一起：exe 依赖 Qt5*.dll、platforms\qwindows.dll 和 MinGW 运行时，
  单独把 exe 拷到别处会报 "could not find or load the Qt platform plugin"。

二、授权（可选）
  未授权也能建模、也能求解，但导出的云图与 PDF 会打水印。
  1. 菜单「授权 → 激活 / 查看机器码…」，把窗口里的机器码发给我们；
  2. 收到 license.qglic 后，在同一个对话框点「导入授权」；
  3. 授权与这台机器绑定，换机需要重新签发。

三、这一版是什么
  当前是内置示意后端（MOCK）：数值按经验式估算，趋势与量级合理，
  但不是有限元结果，界面会显示「示意数据」角标。不能用于工程设计验证或安全认证。

四、出问题时怎么报
  菜单「帮助 → 关于」里能看到版本号。反馈时请把这一行贴在问题里，
  说明操作步骤和现象；有条件的话附上 视图 → 求解日志 的内容。
"@
$readmePath = Join-Path $target "使用说明.txt"
# 带 BOM：Windows 记事本按 ANSI 猜编码时不会把中文糊掉
[System.IO.File]::WriteAllText($readmePath, $readme, (New-Object System.Text.UTF8Encoding $true))

# ---- 5. 压缩 ----
$zip = Join-Path $distRoot ("QuickGear-{0}-win64.zip" -f $Version)
if (-not $SkipZip) {
    if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
    Compress-Archive -Path (Join-Path $target "*") -DestinationPath $zip -CompressionLevel Optimal
}

$dirBytes = (Get-ChildItem -LiteralPath $target -Recurse -File | Measure-Object -Property Length -Sum).Sum
$line = "解包 {0:N1} MB" -f ($dirBytes / 1MB)
if (-not $SkipZip) {
    $zipBytes = (Get-Item -LiteralPath $zip).Length
    $line += ("，zip {0:N1} MB" -f ($zipBytes / 1MB))
}
$fileCount = $entries.Count + 1   # 加一份使用说明.txt
Write-Host ("打包完成：{0}`n{1} · {2} 个文件" -f $target, $line, $fileCount)
