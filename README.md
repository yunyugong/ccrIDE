# ccrIDE

轻量 C++ IDE，使用 **Dear ImGui + GLFW + OpenGL**。当前版本：**0.3.1**。
产品方向是轻量、离线、单文件优先的 C++ IDE，按需通过插件扩展工程能力。
当前提供代码补全、语法高亮、多文件标签、独立编译/运行及基础 GDB 调试。样例测试和对拍仍待实现。

[总体需求](docs/product-requirements.md) · [现状核对、架构约定与分批验收清单](docs/implementation-plan.md)

![构建](https://img.shields.io/badge/构建-g%2B%2B%2011%2B-blue) ![依赖](https://img.shields.io/badge/依赖-E%3A%5CcPP-orange)

---

## v0.3.1 插件安装修复

- 本地插件在目标盘暂存，解决系统临时目录与安装目录不在同一盘时安装失败。
- 线上安装保留接口版本；校验清单路径，下载与写入成功后再完成安装，失败清理临时文件。
- 加载失败显示具体原因，鼠标悬停状态查看详情；缺少入口的 DLL 立即释放。
- 增加真实 DLL 的线上安装、跨盘安装与失败恢复检查。兼容 v0.3.0 的两个独立插件。

## v0.3.0 离线安装与可选插件

`ccrIDE-Setup-v0.3.0.exe` 是本项目编写的离线安装程序，内含 GCC 11.5 和 GDB，可选择目录、查看进度和取消安装。已有安装目录不会被覆盖，也提供同内容的便携 ZIP。

主程序保留普通代码补全、类型和函数签名提示。以下两项从主程序中移出，分别在“插件 → 安装本地插件”选择 ZIP 安装：

- `ai-assistant-v1.0.0.zip`：AI 对话、选中代码、插入回复，以及独立的连接设置。
- `completion-docs-v1.0.0.zip`：补全注释全文、来源头文件切换和可点击网址。

两个插件可各自停用或卸载；不安装时普通代码补全仍可用。AI 设置由插件保存到 `data/plugins/ai-assistant/settings.json`，首次安装可迁移本机旧配置。分发包不携带密钥。插件需要 API v2 / ccrIDE 0.3.0 或以上。

构建文件名只包含版本和必要的递增编号，旧产物保留；构建时间保存在关于页面与 build-info.json，不写入 EXE 文件名。

完整构建：`cmake --build build/cmake --target MiniCPP ai_assistant_plugin completion_docs_plugin ccride_installer`。分别使用 `packaging/package-plugins.py --output <新目录>` 和 `packaging/package-portable.py --stage <新暂存目录> --output <新目录> --settings <本机设置文件>` 生成插件及离线安装包。使用已有配置的 CMake 构建目录，依赖路径见后文。

## 一、功能一览

| 功能 | 说明 |
|---|---|
| 编辑器 | 自绘代码编辑器: 行号、选区、光标闪烁、撤销/重做(800步)、Tab=4空格、Enter 智能缩进(`{` 加缩进 / `}` 减缩进)、自动括号配对、智能退格 |
| 语法高亮 | C/C++ 关键字、类型名、字符串、注释(含块注释跨行)、数字、预处理指令; 深色/浅色两套主题 |
| 代码补全 | 输入时自动弹出 (或 `Ctrl+Space`)。**两套语义模型** (对齐小熊猫C++): ① **文档模型** (`code_model`) 作用域感知 —— 只提示光标处真正可见的局部变量/函数参数/全局函数与类型; ② **头文件模型** —— 解析本文件 `#include` 的**所有头文件** (递归内联, 仿小熊猫 `CppPreprocessor`), `std::`、类型成员、函数签名**全部来自真实头文件解析**, 未包含的头文件不提示 (不再依赖手写符号表)。`.`/`->` 成员补全 (自定义类型含基类链, 支持链式 `a.b->c`、`this->x`、`auto` 推断); `#include <` 弹头文件; 大小写不敏感前缀 + 子序列/缩写模糊匹配 (`pb`→`push_back`); 字符串/注释内不弹出; 函数候选接受后自动补 `()` |
| 参数签名提示 | 输入 `(` 弹出 calltip, 随 `,` 高亮下一个参数; 签名优先取自文档/头文件解析结果 (真实参数列表), 系统库函数兜底 |
| 查找 | `Ctrl+F`: 高亮全部匹配、上一/下一个、计数 |
| 多文件 | 标签页, 可新建/打开/关闭(带未保存确认), 未保存标记 `*` |
| 编译 / 运行 | 后台线程: `g++ -std=c++17 -O2`, 编译错误红色显示; 输出增量显示，支持在输出面板输入 stdin 和终止运行 |
| 自动保存 | 每 3 秒写入 `数据盘\MiniCPP\autosave.cpp`, 启动时询问是否恢复 |
| 备份 | `Ctrl+Alt+S` → `数据盘\MiniCPP\backup\backup_YYYYMMDD_HHMMSS.cpp` |
| 插件市场 | 扫描 `E:\cPP\plugins\<id>\` 下的 DLL 插件; 可配置线上市场 JSON 下载安装; 随附"时钟/示例"两个插件 |
| AI 助手（独立插件） | 按需安装；OpenAI 兼容接口 (Ollama / 任意兼容服务), 流式/非流式多轮对话，支持停止生成及附带选中代码 (实验性) |
| 设置 | g++ 路径、主题、字号、市场地址；AI 配置由插件独立保存 |

设置 → 代码辅助：补全与参数提示、语法高亮、自动语法检查分别启停；关闭高亮仍保留分词、导航、折叠与括号匹配；兼容旧配置。

设置 → 运行与资源限制：可设置时间、提交内存和总输出上限，以及程序参数、目录和环境覆盖。停止按钮可取消编译或运行。编译、交互运行及进程工具共用执行后端，支持受控进程组清理、有界输出和异步输入。详见 [执行管理与验证边界](docs/execution-manager.md)。

### 旧配置兼容
- 数据盘选择: **D > E > F**, 禁止 C, 测试可写性后使用第一个可写盘
- g++ 自动查找: `D:\rred\mingw64\bin`、`E:\rred\mingw64\bin`、Dev-C++ 目录, 递归搜索 `g++.exe`
- 文件读写 UTF-8

---

## 二、目录结构

```
E:\cppIDE\
├── build.ps1              一键构建 (EXE + 内置插件清单)
├── src\
│   ├── main.cpp           入口 / GLFW+ImGui / 停靠布局 / 菜单 / 主循环
│   ├── app.h / app.cpp    全局状态 / 控制台 / 初始化 / F11 管线 / 自动保存
│   ├── editor.h/.cpp      自绘代码编辑器 (高亮/补全/查找/撤销/智能缩进)
│   ├── code_model.h/.cpp  文档级 C++ 语义模型 (作用域符号表/类型推断/成员/签名)
│   ├── symbol_index.h/.cpp  系统头文件符号索引 (补全/成员兜底)
│   ├── plugin_api.h       插件 C API (宿主与插件共享)
│   ├── plugin_manager.h/.cpp  插件扫描/加载/市场
│   ├── config.h/.cpp      设置 (nlohmann JSON)
│   ├── utils.h/.cpp       UTF-8 / 进程 / 路径 / HTTP (WinINet)
│   └── ui_helpers.h       std::string 版 InputText
├── plugins\
│   ├── ai-assistant\      独立 AI 助手 DLL 插件
│   ├── completion-docs\   独立补全注释 DLL 插件
│   ├── clock\             示例插件: 时钟
│   └── hello\             示例插件: Hello
└── build\MiniCPP.exe      构建产物

E:\cPP\                     第三方依赖 (第三方库必须放这里)
├── imgui\                  Dear ImGui v1.92.9b (docking 分支)
├── glfw\                   GLFW 3.4 (已编译 libglfw3.a)
└── nlohmann\nlohmann\json.hpp
```

---

## 三、构建与运行

```powershell
# 在 E:\cppIDE 下执行
powershell -ExecutionPolicy Bypass -File build.ps1
```

产物:
- `E:\cppIDE\build\MiniCPP.exe`
- 时钟和 Hello 示例插件内置于 EXE；清单复制到 `E:\cPP\plugins\<id>\plugin.json`，无需单独 DLL

也可以在 Red Panda C++ 中新建工程, 添加 `src\*.cpp` 并配置:

```
编译器选项:  -std=c++17 -O2 -mwindows -static-libgcc -static-libstdc++
包含目录:   E:\cPP\imgui; E:\cPP\imgui\backends; E:\cPP\glfw\include; E:\cPP\nlohmann; src
链接库:     E:\cPP\glfw\libglfw3.a, -lopengl32 -lgdi32 -luser32 -lshell32
            -lwininet -lcomdlg32 -limm32 -lole32 -ladvapi32
源文件:     src\*.cpp + E:\cPP\imgui\imgui.cpp imgui_draw.cpp imgui_tables.cpp
            imgui_widgets.cpp imgui_impl_glfw.cpp imgui_impl_opengl3.cpp
```

> ⚠ 依赖已固定在 E:\cPP; 若删除可重新下载:
> - imgui: `https://github.com/ocornut/imgui/archive/refs/tags/v1.92.9b-docking.zip`
> - glfw:  `https://github.com/glfw/glfw/archive/refs/tags/3.4.tar.gz` (需先 `build.ps1` 前手动编译 libglfw3.a)
> - json:  `https://raw.githubusercontent.com/nlohmann/json/develop/single_include/nlohmann/json.hpp`

---

## 四、快捷键

| 按键 | 功能 |
|---|---|
| `F11` | 保存 → 编译并交互运行（可终止） |
| `Ctrl+S` / `Ctrl+Shift+S` | 保存 / 另存为 |
| `Ctrl+Alt+S` | 备份到 backup 目录 |
| `Ctrl+N` / `Ctrl+O` | 新建 / 打开 |
| `Ctrl+Z` / `Ctrl+Y` | 撤销 / 重做 |
| `Ctrl+C/X/V/A` | 剪切 / 复制 / 粘贴 / 全选 |
| `Ctrl+F` | 查找 (Enter 下一个, Shift+Enter 上一个) |
| `F12` / `Ctrl+左键` | 跳转到定义 (语义模型: 局部变量/参数/函数/类型/成员; 成员访问 `obj.member` 亦可) |
| `Ctrl+Space` | 手动触发代码补全 |
| `Tab` | 缩进 4 空格; 补全弹出时接受候选 |
| `Enter` | 智能换行; 补全弹出时接受候选 |
| `Esc` | 关闭补全 / 查找 |

---

## 五、AI 助手配置 (实验性)

1. 菜单 `AI → AI 设置`
2. 勾选"启用 AI", 填:
   - 接口地址: 如 Ollama 本地 `http://127.0.0.1:11434/v1`
   - API Key: 留空 (Ollama) 或填写
   - 模型名: 如 `qwen2.5-coder:7b`
3. 点"保存设置", 打开 `AI 助手`, 可"测试连接"
4. 提示框输入需求, 可勾选"附带编辑器选中代码"

> 生成图片: 你有 ComfyUI, 后续可在插件市场/AI 面板中集成图片生成 (非本次范围)。

---

## 六、插件开发

插件目录为 `E:\cPP\plugins\<id>\`。外部插件包含 DLL 与 `plugin.json`；内置插件仅需清单。以下是外部插件清单示例：

```json
{
  "id": "myplug", "name": "自定义插件", "version": "1.0",
  "author": "MiniCPP", "description": "外部插件示例",
  "entry": "myplug.dll", "kind": "soft", "bundled": false
}
```

DLL 需导出三个 C 函数 (见 `src/plugin_api.h`):

```cpp
extern "C" __declspec(dllexport) void Plugin_OnLoad(void* imguiCtx, PluginApi* api);
extern "C" __declspec(dllexport) void Plugin_OnFrame(void);
extern "C" __declspec(dllexport) void Plugin_OnUnload(void);
```

以上为外部 DLL（`kind: "soft"`）接口。内置示例使用 `kind: "hard"`，入口带 `Clock_` / `Hello_` 前缀，由 `builtin_plugins.cpp` 注册，随主程序链接。

在 `Plugin_OnLoad` 中调用 `ImGui::SetCurrentContext((ImGuiContext*)imguiCtx)` 即可用宿主 ImGui 绘制窗口。
插件须与宿主使用**相同版本**的 imgui 头文件编译, 并链接自己的 imgui 副本。

> ⚠ **插件窗口务必加 `ImGuiWindowFlags_NoFocusOnAppearing`** (见示例插件),
> 否则窗口首次出现会抢走主编辑器的键盘焦点, 导致无法输入。

外部 DLL 插件须实现上述通用导出接口，并单独编译成 DLL；不能直接用内置示例源码生成外部插件。`build.ps1` 和 CMake 均将两个示例编入主程序，并复制其清单。

> ⚠ **重要**: 插件内 `PushFont/PushStyleVar/PushID` 等**必须成对 Pop**, 否则污染宿主 ImGui 状态
> 栈, 触发 `Missing PopFont()` 断言。宿主 context 是所有插件共享的。

线上市场: 在设置中填一个 JSON 地址, 形如:

```json
[
  {"id":"myplug","name":"我的插件","version":"1.0","author":"x",
   "description":"...","entry":"myplug.dll","dllUrl":"https://.../myplug.dll"}
]
```

---

## 七、常见问题

| 问题 | 处理 |
|---|---|
| 启动报"没有可写入的数据盘" | 需要 D/E/F 中至少一个可写盘 |
| 状态栏显示"未找到 g++" | 设置 → g++ 路径 → 自动查找, 或手动填 `E:\rred\mingw64\bin\g++.exe` |
| 编译失败无输出 | 确认文件已保存 (F11 会自动另存为) |
| 程序卡死 (输出面板无反应) | 可在输出面板输入数据或点击终止运行；有图形界面的程序不适合在本 IDE 内运行 |
| 插件市场为空 | 默认只有随附插件; 线上市场需在设置里配置地址 |

---

## 八、技术要点

- **渲染**: GLFW 3.4 + OpenGL 3.3 Core + Dear ImGui 1.92.9 (docking 分支), 自绘 UI
- **字体**: 微软雅黑 (UI) + Consolas 合并中文字形 (编辑器等宽字体), 中文注释/字符串正常显示
- **进程**: 统一 `execution::Run`，挂起创建 + Windows Job + 受限继承句柄 + 有界输出管道 + 异步 stdin；超时、取消与退出清理同组后代，编译日志有界落盘
- **网络**: WinINet (`WinINet.h`), 支持 http/https
- **编码**: 全链路 UTF-8 ↔ UTF-16 转换; 文件读写 UTF-8 (与原版 Python 一致)
- **持久化**: `imgui.ini` 保存窗口布局; `settings.json` 保存设置; 自动保存每 3s

### 已知限制 (v1.0)
- 代码补全为**轻量语义模型** (非完整 C++ 语法分析): 文档模型做作用域/类型推断; 头文件模型递归内联并解析 `#include` 的头文件 (有 12MB 总量上限)。**不展开宏**, 对 `#if` 条件分支、复杂模板的推断有限
- 未配置编译器 (拿不到 include 路径) 时无法解析头文件, 库补全回退到内置表
- AI 默认流式输出，可切换非流式请求，支持多轮对话和停止生成
- 不支持跨行字符串字面量高亮 (raw string 除外, 同样未特殊处理)
- 单文件保存为 LF 换行 (打开时自动归一化 CRLF→LF)

## 补全说明与验证

- 普通词、跨标签全局符号、命名空间、对象成员、类型限定成员补全均显示解析得到的类型或函数签名。
- 当前候选的完整签名/类型及注释显示在列表下方；说明区限高并可滚动，弹窗按可用屏幕空间调整位置。
- 注释中的 `http://`、`https://`、`www.` 网址显示为可点击链接（包括换行后的网址片段），悬停显示地址，点击使用系统默认浏览器打开。
- 注释从声明紧邻上一行提取，支持连续 `//`、`///`、`//!` 和完整 `/* ... */` 块；空行、宏、其他代码会中断关联。不会展开宏或解释 Doxygen 指令。
- 类型/成员/注释来自轻量 C++ 解析，复杂模板或条件编译下可能缺失；无可靠关联时说明留空。
- 编辑停顿后可自动语法检查；支持交互运行、最近文件、会话恢复、文件树及可配置快捷键。

两种构建入口均包含内置插件。CMake 的依赖路径及 `PLUGIN_ROOT` 可覆盖；回归测试不打开原生窗口，包含补全来源、注释边界和 ImGui 弹窗布局检查。

```powershell
cmake -S . -B build/cmake -G "MinGW Makefiles" -DBUILD_TESTING=ON
cmake --build build/cmake -j 4
ctest --test-dir build/cmake --output-on-failure
```

## 工作区布局

顶部采用 34 像素紧凑图标工具栏，分组放置新建、打开、保存、另存为、备份、撤销/重做、查找、运行与停止，以及面板开关。悬停显示名称与快捷键，按钮按状态启用。编译配置下拉框支持 Release（-O2）和 Debug（-g -O0），保留附加参数；Debug 配置不提供断点调试器。窄窗口精简次要按钮，其余操作可通过菜单使用。文件标签内置关闭按钮，支持中键关闭和右键菜单；未保存确认保持有效。

默认布局为左侧文件、中间编辑器、下方输出，AI 助手按需显示。状态栏显示诊断、行列和编码，完整路径可悬停查看。停靠布局会保留，使用“视图 → 恢复默认布局”重新排列。深色与浅色主题统一按钮、标签、间距和面板配色。

真实界面的离屏预览与交互检查（需要本机 OpenGL）可手动运行：

```powershell
cmake --build build/cmake --target ui_preview -j 4
build/cmake/ui_preview.exe build/ui-previews
```

## 编辑器选区与配色

多行选区在选中换行时延伸到视口行尾，空行连续高亮；选中文字有独立前景色。支持 Shift+点击扩展选区、行号栏整行选择、双击按词拖选，以及拖出视口后自动滚动。Tab、UTF-8 字符及横向滚动使用实际字形宽度。

“设置 → 编辑器配色与辅助线”提供编辑器、语法和诊断三组颜色，以及彩虹括号、缩进提示线、彩虹缩进线、彩虹缩进填充和空白标记开关。颜色实时预览，深浅主题分别保存，支持恢复单项或当前主题默认色；点击“保存设置”持久化。活动断点/断点颜色可编辑并保存，当前只预留配色，尚无断点调试器。


## ccrIDE v0.2.0：构建、调试和行标记

- 默认 F10 编译，F11 编译并运行，F12 运行当前文件最近一次成功编译的程序（不隐式重新编译）。Ctrl+B 或 Ctrl+点击跳转定义。支持在设置中自定义快捷键。
- F5 开始调试，暂停后 F5 继续。Ctrl+点击行号或编辑器右键切换断点；调试面板提供步入、步过、暂停、停止、局部变量、调用栈和表达式求值。无断点时暂停在 main。
- 调试使用独立的 `-g -O0` 构建；GDB 路径留空时使用 g++ 同目录中的 gdb.exe。输入从设置中的文件重定向，留空提供 EOF。普通运行支持交互输入。
- 行号旁红杠：相对上次保存新增或修改的行；绿杠：原有或已保存行。保存后红杠转绿，撤销/重做同步更新。配色可在设置中修改。大量重写时比较有计算上限，不能确定的变化区域保守标红。
- 用户程序产物放在源文件旁 `.ccrIDE-build`，文件名包含唯一时间标识。每次成功编译都保留旧 EXE，产物索引保存在数据目录。断点随有路径的标签会话保存。
- `build.ps1 -Version 0.2.0` 把发布程序写入 `build/releases/ccrIDE-v0.2.0-build<日期时间>.exe`，不会覆盖原有 MiniCPP.exe 或任何旧版本发布程序。窗口标题、“关于”和发布元数据均有版本与构建时间。开发 CMake 链接使用私有 .bin，再发布独立版本 EXE。
- GCC 运行 DLL 与发布程序放在同一目录；便携包包含工具链、GDB、SDK 和设置。最新发布元数据在 `build/releases/latest-build.json`。

调试边界：当前基础 GDB/MI 会话是单目标、以暂停线程和栈帧检查为主，未实现线程管理、寄存器和反汇编。使用 Windows 本地调试器暂停目标，GDB 命令超时后清理会话；GDB 和调试目标共享受控进程组。调试不套用普通运行的墙钟/内存上限，输出总量受限。修改源码后应重新启动调试以对应新的符号；当前不支持交互调试 stdin，使用输入文件。未声称已覆盖所有 GDB 版本和字符集；随包 GDB 10.2 已验证中文与空格目录。

版本沿革：最初原型是 `IDE.py`；随后经历 MiniCPP 阶段。当前产品名为 **ccrIDE**，当前简介不再使用“从 IDE.py 改写”作为产品定位。


## ccrIDE v0.2.1：工作区布局与系统文件图标

- 左侧文件与工具入口、中央编辑器、底部问题/输出/终端/调试使用原生 ImGui 停靠窗口。标签仍可拖动、拆分、浮动，并保存布局；旧布局不会被强制重置。视图菜单可恢复默认布局。
- 构建日志与程序终端分开，运行输出及标准输入进入“终端”；问题列表可点击跳转。调试检查区并排显示局部变量和调用栈，保留断点及日志。
- 文件树通过 Windows Shell 获取真实文件关联图标，未关联类型使用系统通用图标。缓存并只查询可见行，刷新文件树时重新获取；不修改文件关联。
- 工具栏突出编译运行，保留 F10 / F11 / F12 / F5；设置按六类组织，窄窗口使用分类下拉框。
- 发布程序仍包含版本号、构建时间并使用独立文件名，不覆盖旧版本。

- 普通单词与未限定成员补全按当前命名空间和 using 声明筛选；局部 using 离开作用域后失效。只有限定名或合法导入才提示 std::cin 等符号。
- 补全文档合并 Doxygen 分组说明与声明行末注释，保留长注释和段落，不再限制为 120 行。详情显示声明文件和行号；命名空间重开、类型前置声明等多个来源可在下拉框中切换，悬停显示完整路径。未附注释的来源明确显示无注释。
