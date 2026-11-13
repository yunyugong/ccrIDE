# Windows 源码构建

需要 CMake 3.24 或以上、MinGW-w64 GCC、匹配的静态 zlib；启用测试时需要 Python 3。目前面向 Windows/MinGW-w64，不宣称支持其他平台或 MSVC。

## 依赖布局

默认在源码根目录的 dependencies 中查找：

- imgui/：Dear ImGui 1.92.9b docking 源码，包含 backends/。
- glfw/：GLFW 3.4；包含 include/GLFW/，静态库放在 lib/libglfw3.a 或 glfw/ 下。
- nlohmann/：包含 nlohmann/json.hpp，也支持 include/nlohmann/json.hpp。

可通过 CCRIDE_DEPS_DIR 指定统一根目录，或分别设置 IMGUI_DIR、GLFW_DIR、NLOHMANN_DIR。GLFW 支持 find_package(glfw3 CONFIG) 和 CMAKE_PREFIX_PATH。zlib 使用 find_package(ZLIB)，优先检查所选 GCC 的自带目录；外部 zlib 可设置 ZLIB_ROOT 或 ZLIB_LIBRARY/ZLIB_INCLUDE_DIR。

依赖架构与运行时必须与编译器匹配。项目不会自动下载依赖；ImGui 必须是与插件一致的 docking 源码，缺失依赖或误用普通 master 分支会在配置时明确报错。

## 配置与构建

把 CMake 和编译工具目录加入当前终端 PATH，在源码根目录执行：

    cmake -S . -B build/cmake -G "MinGW Makefiles" -DBUILD_TESTING=OFF
    cmake --build build/cmake --target MiniCPP ai_assistant_plugin completion_docs_plugin clangd_plugin ccride_installer -j 4

CMAKE_CXX_COMPILER 可指定 g++.exe。切换编译器使用新构建目录。原有显式目录参数继续有效，切换依赖版本也建议使用新目录。

默认使用 Release（GCC `-O3 -DNDEBUG`），避免空 BuildType 产生未优化的发布程序。需要调试 IDE 本身时，配置 `-DCMAKE_BUILD_TYPE=Debug`，或传入 `build.ps1 -BuildType Debug`。显式选择的构建类型会被保留。

build.ps1 委托同一套 CMake，支持 -Compiler、-Dependencies、-ImGuiDirectory、-GlfwDirectory、-JsonDirectory、-BuildDirectory 和 -Test，不再维护固定路径或手写链接命令。

主程序发布到 build/releases。内置插件清单默认写入 build/releases/plugins，对应开发版可执行文件旁的 plugins/。原来开发目录的插件需复制过来，旧目录不会被删除。便携包目录规则保持不变。

## 测试与离线打包

    cmake -S . -B build/cmake -DBUILD_TESTING=ON
    cmake --build build/cmake -j 4
    ctest --test-dir build/cmake --output-on-failure

Python 由 find_package(Python3 COMPONENTS Interpreter) 查找，可通过 Python3_EXECUTABLE 覆盖；测试关闭时不要求 Python。

离线打包脚本要求显式提供 --compiler；--dependencies 默认使用源码目录的 dependencies。离线包需要含许可文件的完整依赖源码及完整 MinGW 工具链，仅安装系统开发包不一定满足分发条件。安装外壳必须附加有效载荷后才可用于安装。

参考：[FindZLIB](https://cmake.org/cmake/help/latest/module/FindZLIB.html)、[FindPython3](https://cmake.org/cmake/help/latest/module/FindPython3.html)。

## 可选 clangd 离线包

clangd_plugin 构建语言服务适配 DLL。默认不下载 clangd，用户可在插件设置中指定已安装的服务。打包完整离线插件时，配置 CCRIDE_CLANGD_BUNDLE 指向官方发行版根目录（含 bin/clangd.exe、lib/ 和 LICENSE.TXT），重新构建 clangd_plugin，再运行 package-plugins.py。整个发行版会复制到插件 toolchain/，包含资源头文件与许可证。

本次离线包使用官方 clangd 23.1.0，源归档 SHA256：23412a240756a162e7b98a282f36aa2a23a88db5ce16a0cbc4fef7253768c810。测试目标 clangd_tests 自动查找服务，也可设置 CCRIDE_TEST_CLANGD；未找到服务时 CTest 显示跳过，不作为通过。
