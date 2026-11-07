#pragma once
// MiniCPP - 应用全局状态
#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <atomic>
#include <thread>
#include <deque>
#include <imgui.h>
#include "config.h"
#include "editor_style.h"
#include "utils.h"
#include "editor.h"
#include "plugin_manager.h"
#include "debugger.h"
#include "recovery.h"

enum class BuildAction { Compile, BuildRun, Run, Debug };

struct AppColors {
    ImU32 editorBg, curLineBg, gutterBg, lineNum, selection, findMatch;
    ImU32 text, keyword, typeName, str, comment, number, preproc, caret;
    ImU32 scrollTrack, scrollThumb;
    ImU32 diagErrBg, diagWarnBg, diagErrMark, diagWarnMark;   // 语法诊断行标注
    ImU32 bracketBg;                                          // 括号匹配高亮
};

struct ConsoleLine { ImU32 color; std::string text; };

struct Console {
    // deque: 尾部追加 + 头部裁剪均为 O(1) (vector::erase(begin) 在超过上限后每次 O(n))
    std::deque<ConsoleLine> lines;
    static constexpr size_t MAX_LINES = 6000;
    static constexpr size_t MAX_BYTES = 8 * 1024 * 1024;
    size_t textBytes = 0;
    bool autoScroll = true;
    std::string stdinBuf;                       // 运行中的 stdin 输入框缓冲
    void Append(const std::string& t, ImU32 c);
    void Clear();
    void DrawWindow(bool* open, bool terminal = false);
};

struct ProcOut { std::string text; ImU32 color; bool terminal = false; };

struct App {
    std::thread compilerScanThread;
    std::atomic<bool> compilerScanBusy{false},compilerScanCancel{false},compilerScanDone{false};
    std::vector<std::wstring> compilerScanResults,compilerCandidates;
    bool compilerPickerOpen=false;
    std::string compilerScanMessage;
    void StartCompilerSearch(const std::vector<std::wstring>& roots = {});
    void PumpCompilerSearch();
    bool SelectCompiler(const std::wstring& path);
    void DrawCompilerPicker();
    // ---- 路径 ----
    std::wstring saveDir;       // D:\ etc.
    std::wstring dataDir;       // saveDir\MiniCPP
    std::wstring backupDir;     // dataDir\backup
    std::wstring settingsPath;
    bool portableMode = false;
    std::wstring executableDir, pluginDir;
    Settings settings;

    // ---- 样式 / 字体 ----
    AppColors colors;
    std::array<ImU32, (size_t)EditorColor::Count> editorPalette = DefaultEditorPalette(true);
    ImU32 EditorColorValue(EditorColor color) const { return editorPalette[(size_t)color]; }
    ImFont* uiFont = nullptr;
    ImFont* editorFont = nullptr;
    bool fontRebuildPending = false;   // 字号变更后下一帧重建字体 (即时生效)

    // ---- 编辑器 ----
    std::vector<std::unique_ptr<CodeEditor>> editors;
    int active = -1;
    int pendingClose = -1;      // 待确认关闭的标签
    std::vector<std::wstring> recentFiles;   // 最近打开 (MRU, 限长 10, recent.json 持久化)
    void LoadRecent();
    void SaveRecent();
    void PushRecent(const std::wstring& path);          // 去重 push_front + 保存
    void OpenRecentFile(const std::wstring& path);      // 从最近列表打开

    // ---- 子系统 ----
    Console console;
    Console terminal;
    PluginManager plugins;
    Debugger debugger;
    DebugSnapshot debugView;
    bool showDebug = false;
    std::mutex artifactMutex;
    std::map<std::wstring,std::wstring> lastArtifacts;
    std::vector<std::pair<std::string,int>> DebugBreakpoints() const;

    // ---- 窗口开关 ----
    bool showConsole  = true;
    bool showProblems = true;
    bool showTerminal = true;
    bool terminalRevealed = false;
    bool focusTerminal = false;
    bool showFileTree = true;   // 文件树
    bool showMarket   = false;
    bool showPlugins  = false;   // 插件管理 (硬编译/DLL 启停)
    bool showSearch   = false;   // 全局查找
    bool showSettings = false;
    bool showAbout    = false;

    // ---- 后台编译/运行 ----
    std::mutex procMutex;
    std::deque<std::vector<ProcOut>> procOutputs;
    size_t queuedOutputBytes = 0;
    bool outputTruncated = false;
    void QueueProcOutput(const std::string& text, ImU32 color, bool terminal = false);
    std::atomic<bool> procBusy{false};
    void SetProcStatus(const std::string& s);   // 后台线程写状态 (加锁, 避免 std::string 数据竞争)
    std::string GetProcStatus();                // 主线程读状态 (加锁)
    std::thread procThread;     // 编译/运行线程 (退出时 join, 避免 detached 线程持有 this 崩溃)
    std::string m_procStatus;   // 受 procMutex 保护; 读写必须经 SetProcStatus/GetProcStatus

    // ---- F5 运行中 stdin 交互 ----
    std::mutex stdinMutex;
    std::vector<std::wstring> stdinQueue;      // 主线程 -> 子进程的待写输入行
    std::atomic<bool> runInteractive{false};   // 子进程运行中, 输出面板显示输入框
    std::atomic<bool> killRequested{false};    // 输出面板 [终止运行] 请求
    std::atomic<bool> shuttingDown{false};
    void QueueStdin(const std::wstring& line); // 把用户输入入队, 后台线程写入子进程 stdin

    // ---- 自动语法检查 (输入停顿 0.8s 后台 g++ -fsyntax-only, 同小熊猫C++) ----
    std::mutex diagMutex;
    std::vector<Diag> diagResults;      // 后台 -> 主线程 (加锁)
    uint64_t editRev = 0;               // 每次编辑递增
    uint64_t diagDoneRev = 0;           // 结果对应的编辑代数
    bool diagAutomatic = true;        // 手动编译诊断不受自动检查开关影响
    std::thread diagThread;
    std::atomic<bool> diagBusy{false};
    bool  diagPending = false;          // 有未检查的编辑
    float diagTimer = 0;                // 停顿计时
    std::string diagStatus;             // 状态栏显示: "2 错误, 1 警告" / "无错误" / "检查中"
    std::vector<Diag> activeDiags;      // 问题面板 (Console 顶部)

    // ---- 自动保存 / 恢复 ----
    float autosaveTimer = 0;
    bool  recoverPending = false;
    bool  recoverIgnore = false;
    std::string recoverContent;
    recovery::Snapshot recoverSnapshot;
    std::vector<unsigned char> recoverSelection;
    void SaveRecovery();
    void ApplyRecovery(bool restore);

    // ---- 初始化 ----
    bool Init();                    // 路径 / g++ / 设置 / 插件 (ImGui 上下文之前可调用)
    void LoadFonts();               // 需在 ImGui::CreateContext 之后
    void ApplyTheme();              // 需在 ImGui 上下文之后

    CodeEditor* ActiveEditor();
    void NewFile(const std::wstring& path = L"");
    void OpenFileDialog();
    bool SaveActive(bool saveAs);
    void CloseEditor(int idx);
    void RemoveEditorAt(int idx);   // 无确认直接移除并修正 active 索引 (CloseEditor 弹窗确认后亦调用)
    void StartCompileRun(BuildAction action = BuildAction::BuildRun);
    void PumpProcResults();
    void MarkDiag();                // 编辑后调用: 标记待检查
    void DiagTick(float dt);        // 主循环: 停顿计时到点启动检查
    void StartDiagCheck();
    void PumpDiagResults();         // 主循环: 应用检查结果
    void SyncDiagPanel();           // 问题面板/状态栏跟随当前标签的诊断
    void GotoEditorLine(int line);  // 问题列表点击跳转
    void OpenAtFile(const std::string& pathU8, int line);  // 输出面板双击 file:line 跳转
    void SaveSession();             // 会话: 打开的标签+光标+滚动 (退出/定期保存)
    void LoadSession();             // 启动时恢复上次会话
    void Shutdown();            // 退出前调用: 等待编译线程与 AI 线程结束
    void AutoSaveTick(float dt);
    void BackupNow();
    void SaveSettings();
};

extern App g_app;
