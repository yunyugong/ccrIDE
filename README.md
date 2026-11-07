# ccrIDE

轻量 C++ IDE，使用 Dear ImGui、GLFW 和 OpenGL。当前版本：**0.3.7**。

0.3.7 新增统一任务 ID、独立运行与临时目录，以及跨实例并发限制；从“运行 → 任务管理”查看和取消任务。使用方法及限制见 [任务调度](docs/task-coordinator.md)。AI 与 clangd 插件请使用 1.2.0 版。

0.3.6 修复恢复备份轮换、测试归档一致性，并收紧 DLL 依赖查找与重复 ID 检查。详情见 [0.3.6 修复记录](docs/reliability-0.3.6.md)。

0.3.5 新增多光标/列编辑、clangd 语义重命名预览、样例/Subtask/对拍和所有标签恢复。用法、验证范围及限制见 [工作流说明](docs/workflows.md)。

定位为轻量、单文件优先的开发环境。高级调试、测试框架集成、大型项目管理仍不完整；重型模板元编程可能超出轻量补全模型的能力。详见 [架构核对与验收边界](docs/architecture-review.md)。

提供代码补全、语法高亮、多文件编辑、独立编译与运行，以及基础 GDB 调试。最初原型保留在 `IDE.py`，当前产品名为 ccrIDE。

[总体需求](docs/product-requirements.md) · [实施计划](docs/implementation-plan.md) · [执行管理与验证边界](docs/execution-manager.md)

## 安装与使用

Windows x64 离线安装包内含 GCC 11.5 和 GDB，可选择安装目录、查看进度和取消安装。便携 ZIP 提供相同的程序文件，解压后运行 `ccrIDE-v0.3.7.exe`。

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
| 补全 | 内置轻量模型；可选 clangd 插件提供编译器语义补全、诊断和定义跳转 |
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
| `ai-assistant-v1.2.0.zip` | OpenAI 兼容接口、流式或非流式对话、多轮历史、附带选中代码和插入回复 |
| `completion-docs-v1.0.0.zip` | 轻量补全的注释全文、声明文件与行号、多个头文件来源切换、可点击网址 |
| `clangd-v1.2.0.zip` | 独立 clangd 服务、嵌套模板语义补全、实时诊断、定义跳转；离线包附带 clangd 23.1.0 |

安装：在“插件 → 安装本地插件”中选择 ZIP，无需手动解压。AI 和注释插件需要 ccrIDE 0.3.0 或以上，使用插件 API v2。clangd 插件需要 ccrIDE 0.3.5 或以上，使用兼容扩展 API v3。

clangd 安装后自动启用；在“插件 → clangd 设置”选择服务路径、编译数据库目录、无数据库时的编译参数和内存上限。优先使用 compile_commands.json；服务不可用时回退轻量补全。完整配置、验证范围及限制见 [clangd 使用说明](docs/clangd.md)。

AI 连接通过“插件 → AI 助手设置”配置，设置独立保存至 `data/plugins/ai-assistant/settings.json`。首次安装可迁移本机旧配置，发布包不携带密钥。使用前需要自行准备可访问的兼容服务。

补全注释插件保留长注释与段落；命名空间重开等多个声明来源可在详情中切换。注释内的网址可由系统默认浏览器打开。

时钟与 Hello 是随主程序编译的示例插件，仍需对应清单。

## 编译器查找与选择

首次启动没有有效配置时，后台递归扫描本机固定磁盘和可移动磁盘，不依赖开发者电脑的盘符或安装目录。权限不足的目录会跳过，不跟随目录链接；扫描可取消。

只找到一个 g++ 且尚未配置时自动使用；找到多个时显示完整路径，由用户选择后保存。已有有效配置不会被扫描结果覆盖。设置中可重新扫描、选择已找到的编译器，也可手动编辑 g++ 路径并保存。网络盘不自动扫描。

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

需要 Windows/MinGW-w64、CMake 3.24 或以上、匹配的静态 zlib，以及 Dear ImGui docking、GLFW 和 nlohmann/json。默认使用源码中的 dependencies/，可指定 CCRIDE_DEPS_DIR 或各依赖目录。GLFW 支持 CMake 安装包发现，Python 测试解释器自动查找。

详细目录布局、参数和离线打包要求见 [构建说明](docs/building.md)。配置阶段不会自动联网下载依赖。

    cmake -S . -B build/cmake -G "MinGW Makefiles" -DBUILD_TESTING=OFF
    cmake --build build/cmake --target MiniCPP ai_assistant_plugin completion_docs_plugin clangd_plugin ccride_installer -j 4

主程序发布到 build/releases，插件清单默认保存在可执行文件旁的 plugins/。构建时间用于“关于”和元数据，版本化 EXE 保留旧产物。build.ps1 委托同一套 CMake，不再维护固定路径。

## 打包与检查

插件包由 `packaging/package-plugins.py --output <新目录>` 生成。

离线包由 `packaging/package-portable.py --stage <新暂存目录> --output <新目录> --settings <本机设置文件>` 生成；编译器与依赖位置可通过 `--compiler`、`--dependencies` 指定。安装界面由项目自行编写，ZIP 解压使用 zlib。

构建测试需启用 BUILD_TESTING，Python 解释器由 CMake 查找，也可通过 Python3_EXECUTABLE 覆盖：

```powershell
cmake --build build/cmake -j 4
ctest --test-dir build/cmake --output-on-failure
```

真实界面的离屏预览与交互检查需要本机 OpenGL：

```powershell
cmake --build build/cmake --target ui_preview -j 4
build/cmake/ui_preview.exe build/ui-previews
```

v0.3.5 增加 clangd 协议、真实服务与宿主衔接检查；八组自动检查通过，并单独检查中文路径下的实际插件安装、成员补全、诊断、定义跳转、服务异常退出、重载和卸载。

## 插件运行边界

当前 DLL 插件在 IDE 主进程内执行，共享内存与 ImGui 上下文。插件的崩溃、内存错误和死循环仍可能影响 IDE，只应加载可信插件。AI 1.1.0 已将 HTTP、SSE 和接口响应解析迁到独立的 ai-service.exe；带有取消、超时、消息和资源上限，并完成服务故障注入检查。这不等于所有 DLL 插件已隔离，也不构成权限沙箱。详见 [进程外服务第一阶段](docs/process-services.md)。

核心保留轻量补全；可选 clangd 插件已接入文档同步、补全、诊断、定义跳转和编译数据库。专业调试、测试框架及大型工程能力不能由现有基础接口推断为已经具备。

## 插件开发

接口定义见 `src/plugin_api.h`。外部 DLL 插件清单示例：

```json
{
  "id": "myplug", "name": "自定义插件", "version": "1.0",
  "author": "ccrIDE", "description": "外部插件示例",
  "entry": "myplug.dll", "kind": "soft", "apiVersion": 2, "bundled": false
}
```

API v3 在冻结的 API v2 基础结构之后追加语言服务回调，导出 Plugin_OnLoadV3；现有 v1/v2 插件保持兼容。语言服务在工作线程处理协议，每帧把结果提交宿主。

API v2 导出 `Plugin_OnLoadV2`、`Plugin_OnFrame` 和 `Plugin_OnUnload`。加载时校验接口版本与结构大小，并设置宿主传入的 ImGui 分配器和上下文；具体实现见两个独立插件目录。旧 API v1 仍使用 `Plugin_OnLoad`。

插件的 ImGui Push/Pop 必须配对，窗口首次出现时应避免抢走编辑焦点。内置时钟与 Hello 使用专用入口注册，不能直接作为外部 DLL 示例。

## 已知边界

- 内置补全使用轻量 C++ 模型；复杂模板可选择 clangd，但正确性仍依赖实际编译参数、头文件和工具链配置。
- clangd 首版未实现引用查找、格式化、代码动作及注释详情解析；未命名文件仍用轻量补全。
- 补全文档的注释关联使用启发式；不能可靠关联时显示无注释。完整类型或注释依赖可解析的头文件。
- 调试为基础单目标 GDB/MI 会话，尚未实现线程管理、寄存器和反汇编。
- 调试输入目前使用设置中指定的文件；普通运行支持交互输入。修改源码后需重启调试。
- 样例/Subtask/对拍已提供首版工作流；交互题、自定义检查器与自动缩减反例尚未实现。

## v0.3.1 修复

- 本地插件在目标盘暂存，修复跨盘安装失败。
- 线上安装保留接口版本；下载和写入成功后再完成安装，失败清理临时文件。
- 插件加载失败可查看具体原因；缺少入口的 DLL 立即释放。
- 继续兼容 v0.3.0 的两个独立插件。
