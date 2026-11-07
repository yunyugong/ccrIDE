param([string]$Version = "0.3.2", [string]$BuildDate = "")
# ============================================================
#  ccrIDE 构建脚本 (Windows PowerShell)
#  需要: E:\rred\mingw64\bin\g++.exe (Red Panda C++) 或任意 MinGW-w64
#  依赖: E:\cPP\imgui  E:\cPP\glfw  E:\cPP\nlohmann
#  用法: powershell -ExecutionPolicy Bypass -File build.ps1
# ============================================================
$ErrorActionPreference = "Stop"

$GPP = "E:\rred\mingw64\bin\g++.exe"
if (-not (Test-Path $GPP)) {
    $GPP = (Get-Command g++ -ErrorAction SilentlyContinue).Source
}
if (-not $GPP) { Write-Error "未找到 g++" }

Write-Host "使用编译器: $GPP"

$IMGUI   = "E:\cPP\imgui"
$GLFW    = "E:\cPP\glfw"
$NLOHMANN = "E:\cPP\nlohmann"
$SRC     = Join-Path $PSScriptRoot "src"
$BUILD   = Join-Path $PSScriptRoot "build"
$RELEASE = Join-Path $BUILD "releases"
$GENERATED = Join-Path $BUILD "generated"

New-Item -ItemType Directory -Force -Path $BUILD,$RELEASE,$GENERATED | Out-Null
$buildMoment = Get-Date
if ($BuildDate) { $buildMoment = [DateTime]::ParseExact(($BuildDate + " " + $buildMoment.ToString("HH:mm:ss.fff")), "yyyy-MM-dd HH:mm:ss.fff", [Globalization.CultureInfo]::InvariantCulture) }
$buildStamp = $buildMoment.ToString("yyyyMMdd-HHmmss-fff")
$buildDisplay = $buildMoment.ToString("yyyy-MM-dd HH:mm:ss") + " +08:00"
$headerText = "#pragma once`n#define CCRIDE_VERSION " + [char]34 + $Version + [char]34 + "`n#define CCRIDE_BUILD_TIME " + [char]34 + $buildDisplay + [char]34 + "`n"
[IO.File]::WriteAllText((Join-Path $GENERATED "ccride_build_info.h"),$headerText,[Text.UTF8Encoding]::new($false))
$privateArtifact = Join-Path $BUILD ("ccride-link-" + [Guid]::NewGuid().ToString("N") + ".bin")

$INC = @(
    "-I$IMGUI",
    "-I$IMGUI\backends",
    "-I$GLFW\include",
    "-I$NLOHMANN",
    "-I$SRC",
    "-I$GENERATED"
)

$IMGUI_SRC = @(
    "$IMGUI\imgui.cpp",
    "$IMGUI\imgui_draw.cpp",
    "$IMGUI\imgui_tables.cpp",
    "$IMGUI\imgui_widgets.cpp",
    "$IMGUI\backends\imgui_impl_glfw.cpp",
    "$IMGUI\backends\imgui_impl_opengl3.cpp"
)

$APP_SRC = Get-ChildItem $SRC -Filter *.cpp | Select-Object -ExpandProperty FullName
# 硬编译插件 (kind=hard): 源码随主程序一起编译链接
$APP_SRC += @(
    (Join-Path $PSScriptRoot "plugins\clock\clock.cpp"),
    (Join-Path $PSScriptRoot "plugins\hello\hello.cpp")
)

$LIBS = @(
    "-L$GLFW",
    "-lglfw3", "-lopengl32", "-lgdi32", "-luser32", "-lshell32",
    "-lwininet", "-lcomdlg32", "-limm32", "-lole32", "-ladvapi32", "-lz"
)

$STD = @("-std=c++17", "-O2", "-mwindows", "-static-libgcc", "-static-libstdc++")

Write-Host "==> ccrIDE v$Version | $buildDisplay"
& $GPP @STD @INC @IMGUI_SRC $APP_SRC @LIBS -o $privateArtifact
if ($LASTEXITCODE -ne 0) { Write-Error "主程序编译失败" }
$artifactName = "ccrIDE-v$Version.exe"
$artifactPath = Join-Path $RELEASE $artifactName
$serial = 0
while (Test-Path -LiteralPath $artifactPath) { $serial++; $artifactPath = Join-Path $RELEASE ("ccrIDE-v$Version-$serial.exe") }
[IO.File]::Move($privateArtifact,$artifactPath)
if (!(Test-Path -LiteralPath (Join-Path $RELEASE "libwinpthread-1.dll"))) { Copy-Item -LiteralPath (Join-Path (Split-Path $GPP) "libwinpthread-1.dll") -Destination $RELEASE }
$stream=[IO.File]::OpenRead($artifactPath)
try { $digest=[Security.Cryptography.SHA256]::Create(); $hash=[BitConverter]::ToString($digest.ComputeHash($stream)).Replace('-','').ToLowerInvariant() }
finally { $stream.Dispose(); if($digest) { $digest.Dispose() } }
@{version=$Version;buildTime=$buildDisplay;exe=$artifactPath;sha256=$hash} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $RELEASE "latest-build.json") -Encoding UTF8
Write-Host "==> 构建完成: $artifactPath"

# ---- 内置硬编译插件 (kind=hard): 源码已随 MiniCPP.exe 编译, 这里仅登记 plugin.json ----
$PLUGIN_ROOT = "E:\cPP\plugins"
foreach ($p in @("clock", "hello")) {
    $dir = Join-Path $PLUGIN_ROOT $p
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    Copy-Item (Join-Path $PSScriptRoot "plugins\$p\plugin.json") $dir -Force
    Write-Host "==> 硬编译插件 $p 已登记 (随 ccrIDE 分发, 无需 DLL)"
}

Write-Host ""
Write-Host "全部构建完成! 运行: $artifactPath"
