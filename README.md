# ccrIDE

轻量 C++ IDE，使用 Dear ImGui、GLFW 和 OpenGL。当前版本：**0.3.1**。

提供代码补全、语法高亮、多文件编辑、独立编译与运行，以及基础 GDB 调试。最初原型保留在 `IDE.py`，当前产品名为 ccrIDE。

[总体需求](docs/product-requirements.md) · [实施计划](docs/implementation-plan.md) · [执行管理与验证边界](docs/execution-manager.md)

## 安装与使用

Windows x64 离线安装包内含 GCC 11.5 和 GDB，可选择安装目录、查看进度和取消安装。便携 ZIP 提供相同的程序文件，解压后运行 `ccrIDE-v0.3.1.exe`。

当前 GitHub 仓库提供源码，安装程序和插件 ZIP 尚未上传到 Releases。

安装包与便携包使用以下相对目录，无需按开发者的盘符创建文件夹：

- `data/`：设置、布局、会话和备份。
- `plugins/`：已安装插件。
- `toolchain/mingw64/`：随包编译器和调试器。

移动时携带整个程序目录。更新请选择新目录，保留旧版本及其设置。运行需要支持 OpenGL 3.3 的环境和可显示中文的系统字体。

## 功能

| 功能 | 说明 |
|---|---|
| 编辑 | 多文件标签、撤销重做、自动括号、智能缩进、查找替换、行号和折叠 |
| 补全 | 作用域感知、头文件解析、命名空间与对象成员、函数签名、模糊匹配 |
| 导航 | include 跳转、类型与符号定义跳转 |
| 编译运行 | 后台编译、交互终端、停止运行、可配置参数及资源限制 |
| 调试 | GDB 断点、步入、步过、暂停、局部变量、调用栈和表达式求值 |
| 外观 | 深浅主题、编辑器配色、字体大小、彩虹括号和缩进辅助线 |
| 工作区 | 左侧文件、中央编辑器、底部问题/输出/终端/调试面板；支持拖动、拆分、浮动和停靠 |
| 文件图标 | 使用 Windows 当前文件关联的图标 |

默认布局可通过“视图 → 恢复默认布局”恢复。布局与快捷键可保存。多行选区选中换行时连续铺到行尾，支持拖出编辑区自动滚动。

行号旁红杠表示相对上次保存新增或修改的行，绿杠表示原有或已保存行；颜色可设置。

## 可选插件

AI 助手与补全注释已从主程序移出，分别安装、停用或卸载。**普通代码补全、类型和函数签名提示无需插件。**

| 插件包 | 功能 |
|---|---|
| `ai-assistant-v1.0.0.zip` | OpenAI 兼容接口、流式或非流式对话、多轮历史、附带选中代码和插入回复 |
| `completion-docs-v1.0.0.zip` | 补全注释全文、声明文件与行号、多个头文件来源切换、可点击网址 |

安装：在“插件 → 安装本地插件”中选择 ZIP，无需手动解压。两个插件需要 ccrIDE 0.3.0 或以上，使用插件 API v2。

AI 连接通过“插件 → AI 助手设置”配置，设置独立保存至 `data/plugins/ai-assistant/settings.json`。首次安装可迁移本机旧配置，发布包不携带密钥。使用前需要自行准备可访问的兼容服务。

补全注释插件保留长注释与段落；命名空间重开等多个声明来源可在详情中切换。注释内的网址可由系统默认浏览器打开。

时钟与 Hello 是随主程序编译的示例插件，仍需对应清单。

## 默认快捷键

| 按键 | 功能 |
|---|---|
| `F10` | 编译 |
| `F11` | 编译并运行 |
| `F12` | 运行最近一次成功编译的程序 |
| `F5` | 开始调试 / 暂停后继续 |
| `Ctrl+B` / `Ctrl+左键` | 跳转到定义 |
| `Ctrl+点击行号` | 切换断点 |
| `Ctrl+N` / `Ctrl+O` | 新建 / 打开 |
| `Ctrl+S` / `Ctrl+Shift+S` | 保存 / 另存为 |
| `Ctrl+Alt+S` | 备份 |
| `Ctrl+Z` / `Ctrl+Y` | 撤销 / 重做 |
| `Ctrl+F` | 查找 |
| `Ctrl+Space` | 手动补全 |
| `Tab` / `Enter` | 补全弹出时接受候选 |
| `Esc` | 关闭补全或查找 |

快捷键可在设置中修改。

## 从源码构建

普通用户使用离线包即可。源码构建需要 Windows、MinGW-w64 GCC、CMake，以及 Dear ImGui docking、GLFW、nlohmann/json 和 zlib。

依赖可放在自选目录。CMake 暂时保留开发环境默认值，因此在其他电脑上构建时应显式传入实际目录。下面的路径仅为示例：

```powershell
cmake -S . -B build/cmake -G "MinGW Makefiles" `
  -DIMGUI_DIR="C:/dependencies/imgui" `
  -DGLFW_DIR="C:/dependencies/glfw" `
  -DNLOHMANN_DIR="C:/dependencies/nlohmann" `
  -DPLUGIN_ROOT="./build/plugins" `
  -DBUILD_TESTING=OFF
cmake --build build/cmake --target MiniCPP ai_assistant_plugin completion_docs_plugin ccride_installer -j 4
```

`GLFW_DIR` 应包含 `include/` 和已编译的 `libglfw3.a`；`NLOHMANN_DIR` 应包含 `nlohmann/json.hpp`。编译工具链需要能找到 zlib 头文件及库，安装程序需要可静态链接的 zlib。

当前使用 GLFW 3.4 和 Dear ImGui 1.92.9 docking 系列。插件必须使用与宿主相同的 ImGui 版本、配置和目标架构。

- 主程序产物：`build/releases/ccrIDE-v0.3.1.exe`。同版本重复构建会增加编号，旧产物保留。
- 独立插件产物：`build/plugins/ai-assistant/` 和 `build/plugins/completion-docs/`。
- 安装程序外壳：`build/cmake/ccride_installer.exe`，需要打包脚本附加程序与工具链后才能用于安装。

`MiniCPP` 是保留的内部构建目标名。`build.ps1` 是历史本机开发入口，完整构建主程序、独立插件和安装程序建议使用 CMake。

构建时间保存在“关于”页面与构建元数据中，不写入发布 EXE 文件名或窗口标题。用户代码每次成功编译的产物保存在源文件旁 `.ccrIDE-build/`，使用唯一名称避免覆盖。

## 打包与检查

插件包由 `packaging/package-plugins.py --output <新目录>` 生成。

离线包由 `packaging/package-portable.py --stage <新暂存目录> --output <新目录> --settings <本机设置文件>` 生成；编译器与依赖位置可通过 `--compiler`、`--dependencies` 指定。安装界面由项目自行编写，ZIP 解压使用 zlib。

构建测试需启用 `BUILD_TESTING`。测试配置中的 Python 解释器路径目前仍需按本机环境调整：

```powershell
cmake --build build/cmake -j 4
ctest --test-dir build/cmake --output-on-failure
```

真实界面的离屏预览与交互检查需要本机 OpenGL：

```powershell
cmake --build build/cmake --target ui_preview -j 4
build/cmake/ui_preview.exe build/ui-previews
```

v0.3.1 已完成六组自动检查、跨盘插件安装、中文目录安装，以及安装后的编译、运行与基础 GDB 调试检查。

## 插件开发

接口定义见 `src/plugin_api.h`。外部 DLL 插件清单示例：

```json
{
  "id": "myplug", "name": "自定义插件", "version": "1.0",
  "author": "ccrIDE", "description": "外部插件示例",
  "entry": "myplug.dll", "kind": "soft", "apiVersion": 2, "bundled": false
}
```

API v2 导出 `Plugin_OnLoadV2`、`Plugin_OnFrame` 和 `Plugin_OnUnload`。加载时校验接口版本与结构大小，并设置宿主传入的 ImGui 分配器和上下文；具体实现见两个独立插件目录。旧 API v1 仍使用 `Plugin_OnLoad`。

插件的 ImGui Push/Pop 必须配对，窗口首次出现时应避免抢走编辑焦点。内置时钟与 Hello 使用专用入口注册，不能直接作为外部 DLL 示例。

## 已知边界

- 补全使用轻量 C++ 语义模型，不是完整编译器；复杂模板、宏和条件编译可能导致缺失或推断不准。
- 补全文档的注释关联使用启发式；不能可靠关联时显示无注释。完整类型或注释依赖可解析的头文件。
- 调试为基础单目标 GDB/MI 会话，尚未实现线程管理、寄存器和反汇编。
- 调试输入目前使用设置中指定的文件；普通运行支持交互输入。修改源码后需重启调试。
- 样例测试与对拍尚未实现。

## v0.3.1 修复

- 本地插件在目标盘暂存，修复跨盘安装失败。
- 线上安装保留接口版本；下载和写入成功后再完成安装，失败清理临时文件。
- 插件加载失败可查看具体原因；缺少入口的 DLL 立即释放。
- 继续兼容 v0.3.0 的两个独立插件。
