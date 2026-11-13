param(
    [string]$Version = "",
    [ValidateSet("Release","Debug","RelWithDebInfo","MinSizeRel")][string]$BuildType = "Release",
    [string]$Compiler = "",
    [string]$Dependencies = (Join-Path $PSScriptRoot "dependencies"),
    [string]$BuildDirectory = (Join-Path $PSScriptRoot "build\cmake"),
    [string]$ImGuiDirectory = "",
    [string]$GlfwDirectory = "",
    [string]$JsonDirectory = "",
    [switch]$Test
)
$ErrorActionPreference = "Stop"
$cmake = (Get-Command cmake -ErrorAction Stop).Source
$configure = @("-S",$PSScriptRoot,"-B",$BuildDirectory,"-G","MinGW Makefiles","-DCMAKE_BUILD_TYPE=$BuildType","-DCCRIDE_DEPS_DIR=$Dependencies","-DBUILD_TESTING=$($Test.IsPresent.ToString().ToUpperInvariant())")
if($Version) { $configure += "-DCCRIDE_RELEASE_VERSION=$Version" }
if($Compiler) { $configure += "-DCMAKE_CXX_COMPILER=$Compiler" }
if($ImGuiDirectory) { $configure += "-DIMGUI_DIR=$ImGuiDirectory" }
if($GlfwDirectory) { $configure += "-DGLFW_DIR=$GlfwDirectory" }
if($JsonDirectory) { $configure += "-DNLOHMANN_DIR=$JsonDirectory" }
& $cmake @configure
if($LASTEXITCODE -ne 0) { throw "CMake 配置失败，请查看缺少的依赖和 docs/building.md。" }
if($Test) { & $cmake --build $BuildDirectory -j 4 }
else { & $cmake --build $BuildDirectory --target MiniCPP ai_assistant_plugin completion_docs_plugin clangd_plugin ccride_installer -j 4 }
if($LASTEXITCODE -ne 0) { throw "构建失败。" }
if($Test) {
    & (Join-Path (Split-Path $cmake) "ctest.exe") --test-dir $BuildDirectory --output-on-failure
    if($LASTEXITCODE -ne 0) { throw "测试失败。" }
}
Write-Host "构建完成。版本化主程序保存在 build/releases；插件保存在 build/plugins。"
