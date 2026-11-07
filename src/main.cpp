#include "language_host.h"
#include "contest.h"
#include "toolbar_icons.h"
#include "version.h"
// ============================================================
//  ccrIDE - C++ IDE (ImGui + GLFW + OpenGL)
//  初始原型为 IDE.py；当前产品为 ccrIDE。
//  依赖: E:\cPP\imgui  E:\cPP\glfw  E:\cPP\nlohmann
// ============================================================
#include <windows.h>
#include <shellapi.h>
#include "app.h"
#include "ui_helpers.h"
#include "builtin_plugins.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <GLFW/glfw3.h>
#include "shell_icons.h"
#include <cstdio>
#include <cctype>
#include <algorithm>
#include <filesystem>
#include <vector>
#include <set>
#include <map>
#include <functional>
#include <shlobj.h>

extern App g_app;   // 定义在 app.cpp

// ---------------- 窗口绘制 ----------------
static void DrawMainMenuBar();
static void DrawEditorWindow();
static void DrawStatusBar();
static void DrawSettingsWindow();
static void DrawAboutWindow();
static void DrawSearchWindow();
static void DrawFileTreeWindow();
static void DrawRecoverModal();
static bool showContest=false;
static void DrawCloseConfirmModal();
static void HandleGlobalShortcuts();
static void DrawWorkspaceToolbar();
static void DrawWorkspaceDock();
static void DrawDebuggerWindow();
static bool g_resetWorkspace = false;
static ShellIcons g_shellIcons;
static void DrawProblemsWindow();
static void DrawActivityBar();
static void DefaultBottomDock() {
    // First-use defaults only: user dragging/splitting and saved dock locations win.
    auto* output = ImGui::FindWindowByName("输出");
    if (output && output->DockId) ImGui::SetNextWindowDockID(output->DockId, ImGuiCond_FirstUseEver);
}

static std::string g_iniPath;   // imgui.ini 的持久存储

int main() {
    HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (!g_app.Init()) return 1;

    if (!glfwInit()) {
        MessageBoxW(nullptr, L"GLFW 初始化失败", L"ccrIDE", MB_ICONERROR);
        return 1;
    }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    std::string title=std::string("ccrIDE v")+CCRIDE_VERSION;
    GLFWwindow* window = glfwCreateWindow(1280, 840, title.c_str(), nullptr, nullptr);
    if (!window) {
        MessageBoxW(nullptr, L"窗口创建失败", L"ccrIDE", MB_ICONERROR);
        glfwTerminate();
        return 1;
    }
    glfwSetWindowSizeLimits(window, 640, 440, GLFW_DONT_CARE, GLFW_DONT_CARE);
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    // 拖放 .cpp/.h/.txt 文件到窗口直接打开
    glfwSetDropCallback(window, [](GLFWwindow*, int count, const char** paths) {
        for (int i = 0; i < count; i++) {
            std::wstring wp = utils::U8ToW(paths[i]);
            bool exists = false;
            for (int k = 0; k < (int)g_app.editors.size(); k++)
                if (_wcsicmp(g_app.editors[k]->m_path.c_str(), wp.c_str()) == 0) { g_app.active = k; exists = true; break; }
            if (!exists) g_app.NewFile(wp);
            g_app.PushRecent(wp);   // 拖放也计入最近打开
        }
        g_app.MarkDiag();
    });

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    // 不启用 NavEnableKeyboard: 自绘编辑器需要独占方向键/Enter/Tab,
    // ImGui 的键盘导航会吞掉这些键导致无法移动光标。
    g_iniPath = utils::WToU8(g_app.dataDir) + "\\imgui.ini";
    io.IniFilename = g_iniPath.c_str();

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 130");

    g_app.LoadFonts();
    g_app.ApplyTheme();
    g_app.plugins.SetBuiltinLookup(BuiltinPluginLookup);   // 硬编译插件入口注册
    g_app.plugins.ScanAndLoad();   // 需在 ImGui 上下文创建之后

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        // 字号变更 -> 重建字体 (须在 NewFrame 之前)
        if (g_app.fontRebuildPending) {
            g_app.fontRebuildPending = false;
            g_app.LoadFonts();
            ImGui_ImplOpenGL3_DestroyDeviceObjects();
        }
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        g_app.DrawCompilerPicker();
        DrawMainMenuBar();

        g_app.debugView=g_app.debugger.Snapshot();
        static unsigned observedStop=0;
        if(g_app.debugView.stopped && g_app.debugView.stopRevision!=observedStop) {
            observedStop=g_app.debugView.stopRevision;
            if(!g_app.debugView.file.empty() && g_app.debugView.line>0) g_app.OpenAtFile(g_app.debugView.file,g_app.debugView.line);
        }
        if(!g_app.debugView.active) observedStop=0;
        DrawWorkspaceToolbar();
        DrawStatusBar(); // Reserve its height before laying out docked panels.
        DrawActivityBar();
        DrawWorkspaceDock();

        // 窗口
        if (g_app.showConsole) g_app.console.DrawWindow(&g_app.showConsole);
        if (g_app.showProblems) { DefaultBottomDock(); DrawProblemsWindow(); }
        if (g_app.showTerminal) { DefaultBottomDock(); g_app.terminal.DrawWindow(&g_app.showTerminal, true); }
        if (g_app.showFileTree) DrawFileTreeWindow();
        if (g_app.showMarket)  g_app.plugins.DrawMarketWindow(&g_app.showMarket);
        if (g_app.showPlugins) g_app.plugins.DrawPluginsWindow(&g_app.showPlugins);
        if (g_app.showSearch)  DrawSearchWindow();
        if (g_app.showSettings) DrawSettingsWindow();
        if (g_app.showAbout)   DrawAboutWindow();
        if (g_app.showDebug) { DefaultBottomDock(); DrawDebuggerWindow(); }
        DrawEditorWindow();

        // 启动后自动聚焦编辑器 (首帧后, 让用户直接打字即可)
        static bool focusEditorOnce = true;
        if (focusEditorOnce) {
            ImGuiWindow* w = ImGui::FindWindowByName("编辑器");
            if (w) ImGui::FocusWindow(w);
            focusEditorOnce = false;
        }

        // 插件每帧
        g_app.plugins.CallFrames();

        DrawRecoverModal();
        language::DrawRename();
        if(showContest)contest::Draw(&showContest);
        DrawCloseConfirmModal();

        HandleGlobalShortcuts();
        g_app.PumpProcResults();
        g_app.PumpDiagResults();
        g_app.DiagTick(io.DeltaTime);
        g_app.AutoSaveTick(io.DeltaTime);

        ImGui::Render();
        glViewport(0, 0, (int)io.DisplaySize.x, (int)io.DisplaySize.y);
        glClearColor(0.08f, 0.08f, 0.09f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    g_app.SaveSession();        // 保存会话 (标签列表 + 光标位置)
    g_app.Shutdown();           // 等待编译/AI 后台线程结束
    g_app.plugins.UnloadAll();
    g_shellIcons.Clear();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    if (SUCCEEDED(com)) CoUninitialize();
    return 0;
}

// ================= 可配置全局快捷键 =================
// 动作名 -> 缺省组合。用户修改项存 g_app.settings.hotkeys (settings.json)。
struct HKDef { const char* action; const char* label; const char* def; };
static const HKDef g_hkDefs[] = {
    { "new",     "新建",            "Ctrl+N" },
    { "open",    "打开",            "Ctrl+O" },
    { "save",    "保存",            "Ctrl+S" },
    { "saveAs",  "另存为",          "Ctrl+Shift+S" },
    { "backup",  "备份",            "Ctrl+Alt+S" },
    { "compile", "编译",            "F10" },
    { "run",     "编译并运行",      "F11" },
    { "runOnly", "运行",            "F12" },
    { "debug",   "开始调试",        "F5" },
    { "nextTab", "下一个标签",      "Ctrl+]" },
    { "prevTab", "上一个标签",      "Ctrl+[" },
};
static const int g_hkCount = (int)(sizeof(g_hkDefs) / sizeof(g_hkDefs[0]));
static bool g_hkRecording = false;   // 设置窗口正在录制组合键时, 暂停全局快捷键响应
static std::string g_hkRecAct;       // 正在录制的动作名 (空 = 未在录制)

// 取动作当前组合 (用户未改则用缺省)
static std::string HotkeyText(const char* action) {
    auto& h = g_app.settings.hotkeys;
    auto it = h.find(action);
    if (it != h.end() && !it->second.empty()) return it->second;
    for (auto& d : g_hkDefs)
        if (strcmp(d.action, action) == 0) return d.def;
    return "";
}
static void SetHotkey(const char* action, const std::string& combo) {
    g_app.settings.hotkeys[action] = combo;
}

// ---- 组合串 <-> ImGuiKey ----
struct HKParse { ImGuiKey key = ImGuiKey_None; bool ctrl = false, shift = false, alt = false; bool ok = false; };

static const char* KeyToken(ImGuiKey k) {
    switch (k) {
        case ImGuiKey_Tab:        return "Tab";
        case ImGuiKey_Space:      return "Space";
        case ImGuiKey_Enter:      return "Enter";
        case ImGuiKey_Escape:     return "Esc";
        case ImGuiKey_Backspace:  return "Backspace";
        case ImGuiKey_Delete:     return "Delete";
        case ImGuiKey_Insert:     return "Insert";
        case ImGuiKey_Home:       return "Home";
        case ImGuiKey_End:        return "End";
        case ImGuiKey_PageUp:     return "PageUp";
        case ImGuiKey_PageDown:   return "PageDown";
        case ImGuiKey_UpArrow:    return "Up";
        case ImGuiKey_DownArrow:  return "Down";
        case ImGuiKey_LeftArrow:  return "Left";
        case ImGuiKey_RightArrow: return "Right";
        case ImGuiKey_KeypadEnter:return "NumEnter";
        case ImGuiKey_LeftCtrl: case ImGuiKey_RightCtrl:
        case ImGuiKey_LeftShift: case ImGuiKey_RightShift:
        case ImGuiKey_LeftAlt:   case ImGuiKey_RightAlt:
            return nullptr;   // 修饰键本身不作主键
        default: break;
    }
    // 字母 / 数字
    if (k >= ImGuiKey_A && k <= ImGuiKey_Z) { static char b[2]; b[0] = (char)('A' + (k - ImGuiKey_A)); b[1] = 0; return b; }
    if (k >= ImGuiKey_0 && k <= ImGuiKey_9) { static char b[2]; b[0] = (char)('0' + (k - ImGuiKey_0)); b[1] = 0; return b; }
    // 功能键
    if (k >= ImGuiKey_F1 && k <= ImGuiKey_F12) { static char b[8]; snprintf(b, sizeof b, "F%d", (int)(k - ImGuiKey_F1) + 1); return b; }
    // 标点
    switch (k) {
        case ImGuiKey_GraveAccent: return "`";
        case ImGuiKey_Minus:       return "-";
        case ImGuiKey_Equal:       return "=";
        case ImGuiKey_LeftBracket: return "[";
        case ImGuiKey_RightBracket:return "]";
        case ImGuiKey_Backslash:   return "\\";
        case ImGuiKey_Semicolon:   return ";";
        case ImGuiKey_Apostrophe:  return "'";
        case ImGuiKey_Comma:       return ",";
        case ImGuiKey_Period:      return ".";
        case ImGuiKey_Slash:       return "/";
        default: break;
    }
    return nullptr;
}

static bool TokenToKey(const std::string& t, ImGuiKey& out) {
    if (t.empty()) return false;
    if (t.size() == 1) {
        char c = t[0];
        if (c >= '0' && c <= '9') { out = (ImGuiKey)(ImGuiKey_0 + (c - '0')); return true; }
        if (c >= 'A' && c <= 'Z') { out = (ImGuiKey)(ImGuiKey_A + (c - 'A')); return true; }
        if (c >= 'a' && c <= 'z') { out = (ImGuiKey)(ImGuiKey_A + (c - 'a')); return true; }
        switch (c) {
            case '`': out = ImGuiKey_GraveAccent; return true;
            case '-': out = ImGuiKey_Minus;       return true;
            case '=': out = ImGuiKey_Equal;       return true;
            case '[': out = ImGuiKey_LeftBracket; return true;
            case ']': out = ImGuiKey_RightBracket;return true;
            case '\\':out = ImGuiKey_Backslash;   return true;
            case ';': out = ImGuiKey_Semicolon;   return true;
            case '\'':out = ImGuiKey_Apostrophe;  return true;
            case ',': out = ImGuiKey_Comma;       return true;
            case '.': out = ImGuiKey_Period;      return true;
            case '/': out = ImGuiKey_Slash;       return true;
            default: return false;
        }
    }
    // 命名键
    struct NN { const char* n; ImGuiKey k; };
    static const NN named[] = {
        { "Tab", ImGuiKey_Tab }, { "Space", ImGuiKey_Space }, { "Enter", ImGuiKey_Enter },
        { "Esc", ImGuiKey_Escape }, { "Escape", ImGuiKey_Escape }, { "Backspace", ImGuiKey_Backspace },
        { "Delete", ImGuiKey_Delete }, { "Insert", ImGuiKey_Insert }, { "Home", ImGuiKey_Home },
        { "End", ImGuiKey_End }, { "PageUp", ImGuiKey_PageUp }, { "PageDown", ImGuiKey_PageDown },
        { "Up", ImGuiKey_UpArrow }, { "Down", ImGuiKey_DownArrow }, { "Left", ImGuiKey_LeftArrow },
        { "Right", ImGuiKey_RightArrow }, { "NumEnter", ImGuiKey_KeypadEnter },
    };
    for (auto& n : named)
        if (_stricmp(t.c_str(), n.n) == 0) { out = n.k; return true; }
    // F1..F12
    if (t.size() >= 2 && (t[0] == 'F' || t[0] == 'f')) {
        int v = atoi(t.c_str() + 1);
        if (v >= 1 && v <= 12) { out = (ImGuiKey)(ImGuiKey_F1 + (v - 1)); return true; }
    }
    return false;
}

static HKParse ParseHotkey(const std::string& s) {
    HKParse r;
    std::vector<std::string> parts;
    std::string cur;
    for (char c : s) {
        if (c == '+') { if (!cur.empty()) { parts.push_back(cur); cur.clear(); } }
        else cur += c;
    }
    if (!cur.empty()) parts.push_back(cur);
    bool keySet = false;
    for (auto& p : parts) {
        if (_stricmp(p.c_str(), "Ctrl") == 0) r.ctrl = true;
        else if (_stricmp(p.c_str(), "Shift") == 0) r.shift = true;
        else if (_stricmp(p.c_str(), "Alt") == 0) r.alt = true;
        else { ImGuiKey k; if (TokenToKey(p, k)) { r.key = k; keySet = true; } }
    }
    r.ok = keySet;
    return r;
}

// 动作当前组合是否被按下 (要求修饰键完全匹配)
static bool HotkeyDown(const char* action) {
    HKParse p = ParseHotkey(HotkeyText(action));
    if (!p.ok) return false;
    ImGuiIO& io = ImGui::GetIO();
    if (io.KeyCtrl != p.ctrl || io.KeyShift != p.shift || io.KeyAlt != p.alt) return false;
    return ImGui::IsKeyPressed(p.key);
}

// ---------------- 菜单栏 ----------------
static void DrawMainMenuBar() {
    if (!ImGui::BeginMainMenuBar()) return;

    if (ImGui::BeginMenu("文件")) {
        if (ImGui::MenuItem("新建", HotkeyText("new").c_str())) g_app.NewFile();
        if (ImGui::MenuItem("打开", HotkeyText("open").c_str())) g_app.OpenFileDialog();
        // 最近打开 (MRU)
        if (ImGui::BeginMenu("最近打开", !g_app.recentFiles.empty())) {
            for (size_t i = 0; i < g_app.recentFiles.size(); i++) {
                const std::wstring& p = g_app.recentFiles[i];
                std::string base = utils::WToU8(utils::FileBase(p));
                char buf[8];
                snprintf(buf, sizeof buf, "Ctrl+%d", (int)((i + 1) % 10));
                ImGui::PushID((int)i);
                if (ImGui::MenuItem(base.c_str(), buf)) g_app.OpenRecentFile(p);
                ImGui::PopID();
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", utils::WToU8(p).c_str());
            }
            ImGui::Separator();
            if (ImGui::MenuItem("清空最近记录", nullptr, false, !g_app.recentFiles.empty())) {
                g_app.recentFiles.clear();
                g_app.SaveRecent();
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("保存", HotkeyText("save").c_str())) g_app.SaveActive(false);
        if (ImGui::MenuItem("另存为", HotkeyText("saveAs").c_str())) g_app.SaveActive(true);
        if (ImGui::MenuItem("备份", HotkeyText("backup").c_str())) g_app.BackupNow();
        ImGui::Separator();
        if (ImGui::MenuItem("退出", "Alt+F4")) glfwSetWindowShouldClose(glfwGetCurrentContext(), GLFW_TRUE);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("编辑")) {
        CodeEditor* ed = g_app.ActiveEditor();
        if (ImGui::MenuItem("撤销", "Ctrl+Z", false, ed != nullptr)) ed->Undo();
        if (ImGui::MenuItem("重做", "Ctrl+Y", false, ed != nullptr)) ed->Redo();
        ImGui::Separator();
        if (ImGui::MenuItem("剪切", "Ctrl+X", false, ed != nullptr)) {
            if (ed->m_selActive) { ImGui::SetClipboardText(ed->GetSelectionText().c_str()); ed->DeleteSelection(); }
        }
        if (ImGui::MenuItem("复制", "Ctrl+C", false, ed != nullptr)) {
            if (ed->m_selActive) ImGui::SetClipboardText(ed->GetSelectionText().c_str());
        }
        if (ImGui::MenuItem("粘贴", "Ctrl+V", false, ed != nullptr)) {
            const char* clip = ImGui::GetClipboardText();
            if (clip && *clip) ed->InsertText(clip);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("全选", "Ctrl+A", false, ed != nullptr)) ed->SelectAll();
        if (ImGui::MenuItem("查找", "Ctrl+F", false, ed != nullptr)) {
            ed->m_findOpen = true;
            ed->RecomputeMatches();
        }
        if(ImGui::MenuItem("符号重命名（clangd）",nullptr,false,ed!=nullptr))language::BeginRename(*ed);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("运行")) {
        if (ImGui::MenuItem("编译", HotkeyText("compile").c_str(), false, !g_app.procBusy.load())) { g_app.showConsole=true; g_app.StartCompileRun(BuildAction::Compile); }
        if (ImGui::MenuItem("编译并运行", HotkeyText("run").c_str(), false, !g_app.procBusy.load())) { g_app.showConsole = true; g_app.StartCompileRun(); }
        if (ImGui::MenuItem("运行已编译程序", HotkeyText("runOnly").c_str(), false, !g_app.procBusy.load())) { g_app.showConsole=true; g_app.StartCompileRun(BuildAction::Run); }
        if (ImGui::MenuItem("停止编译或运行", nullptr, false, g_app.procBusy.load())) g_app.killRequested.store(true);
        ImGui::EndMenu();
    }
    if(ImGui::BeginMenu("测试")){if(ImGui::MenuItem("样例、Subtask 与对拍"))showContest=true;ImGui::EndMenu();}
    if(ImGui::BeginMenu("调试")) {
        auto* ed=g_app.ActiveEditor();
        auto debug=g_app.debugger.Snapshot();
        if(ImGui::MenuItem("开始调试",HotkeyText("debug").c_str(),false,!g_app.procBusy.load()&&!debug.active)) { g_app.showDebug=true; g_app.StartCompileRun(BuildAction::Debug); }
        if(ImGui::MenuItem("切换当前行断点","Ctrl+行号",false,ed!=nullptr)) ed->ToggleBreakpoint((int)ed->LineOf(ed->m_cursor)+1);
        if(ImGui::MenuItem("继续",nullptr,false,debug.stopped)) g_app.debugger.Continue();
        if(ImGui::MenuItem("步过",nullptr,false,debug.stopped)) g_app.debugger.Step();
        if(ImGui::MenuItem("步入",nullptr,false,debug.stopped)) g_app.debugger.Step(true);
        if(ImGui::MenuItem("停止调试",nullptr,false,debug.active)) g_app.debugger.Stop();
        ImGui::MenuItem("调试面板",nullptr,&g_app.showDebug);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("视图")) {
        ImGui::MenuItem("文件", nullptr, &g_app.showFileTree);
        ImGui::MenuItem("输出", nullptr, &g_app.showConsole);
        ImGui::MenuItem("问题", nullptr, &g_app.showProblems);
        ImGui::MenuItem("终端", nullptr, &g_app.showTerminal);
        ImGui::MenuItem("全局查找", nullptr, &g_app.showSearch);
        ImGui::MenuItem("插件市场", nullptr, &g_app.showMarket);
        ImGui::MenuItem("插件管理", nullptr, &g_app.showPlugins);
        ImGui::MenuItem("设置", nullptr, &g_app.showSettings);
        ImGui::Separator();
        if (ImGui::MenuItem("恢复默认布局")) { g_resetWorkspace = true; g_app.showFileTree = true; g_app.showConsole = true; g_app.showProblems = true; g_app.showTerminal = true; }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("插件")) {
        if (ImGui::MenuItem("安装本地插件…")) {
            wchar_t path[32768]{}; OPENFILENAMEW dialog{};dialog.lStructSize=sizeof(dialog);
            dialog.lpstrFilter=L"ccrIDE 插件包 (*.zip)\0*.zip\0";dialog.lpstrFile=path;dialog.nMaxFile=32768;
            dialog.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR;
            if(GetOpenFileNameW(&dialog)) { std::string error; if(!g_app.plugins.InstallLocalArchive(path,error)) MessageBoxW(nullptr,utils::U8ToW(error).c_str(),L"插件安装失败",MB_ICONERROR); }
        }
        if (ImGui::MenuItem("插件市场")) g_app.showMarket = true;
        if (ImGui::MenuItem("插件管理", nullptr, &g_app.showPlugins)) g_app.showPlugins = true;
        if (ImGui::MenuItem("重载插件")) g_app.plugins.ReloadAll();
        ImGui::Separator();
        g_app.plugins.DrawCommands();
        ImGui::TextDisabled("已加载 %d 个", g_app.plugins.Count());
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("帮助")) {
        if (ImGui::MenuItem("关于 ccrIDE")) g_app.showAbout = true;
        if (ImGui::MenuItem("打开数据目录")) ShellExecuteW(nullptr, L"open", g_app.dataDir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        ImGui::EndMenu();
    }

    ImGui::EndMainMenuBar();
}

// ---------------- Workspace chrome ----------------
static void DrawWorkspaceToolbar() {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10,7));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10,5));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4);
    if (ImGui::BeginViewportSideBar("##workspace_toolbar", vp, ImGuiDir_Up, 42,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings)) {
        using toolbar::Icon;
        const bool narrow = vp->Size.x < 1000;
        CodeEditor* ed = g_app.ActiveEditor();
        const bool busy = g_app.procBusy.load() || g_app.debugger.Snapshot().active;
        auto tip = [&](const char* name, const char* action) { return std::string(name) + "  " + HotkeyText(action); };
        if (!narrow) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(g_app.settings.theme=="light" ? ImVec4(0.16f,0.39f,0.72f,1) : ImVec4(0.48f,0.70f,1,1), "ccrIDE"); ImGui::SameLine(0,20);
        }
        if (toolbar::Button("##new", Icon::NewFile, tip("新建文件", "new").c_str())) g_app.NewFile();
        if (toolbar::Button("##open", Icon::Open, tip("打开文件", "open").c_str())) g_app.OpenFileDialog();
        ImGui::BeginDisabled(!ed);
        if (toolbar::Button("##save", Icon::Save, tip("保存", "save").c_str())) g_app.SaveActive(false);
        ImGui::EndDisabled();
        if (!narrow) {
            toolbar::Separator();
            ImGui::BeginDisabled(!ed || ed->m_undo.empty());
            if (toolbar::Button("##undo", Icon::Undo, "撤销  Ctrl+Z")) ed->Undo();
            ImGui::EndDisabled();
            ImGui::BeginDisabled(!ed || ed->m_redo.empty());
            if (toolbar::Button("##redo", Icon::Redo, "重做  Ctrl+Y")) ed->Redo();
            ImGui::EndDisabled();
        }
        toolbar::Separator();
        auto& settings = g_app.settings;
        const char* stds[] = { "C++17", "C++20", "C++14", "C++11", "C++23" };
        bool release = !settings.dbgFlag && settings.optLevel == 1 && !settings.useCmdTemplate;
        bool debug = settings.dbgFlag && settings.optLevel == 2 && !settings.useCmdTemplate;
        std::string profile = release ? "Release" : debug ? "Debug" : "自定义";
        if (!settings.extraFlags.empty()) profile += " *";
        std::string label = settings.gppPath.empty() ? "选择编译器…" : "GCC  /  " +
            std::string(stds[std::max(0, std::min(4, settings.stdVersion))]) + "  /  " + profile;
        float comboWidth = narrow ? 132.0f : 238.0f;
        ImGui::SetNextItemWidth(comboWidth);
        ImGui::BeginDisabled(busy);
        if (ImGui::BeginCombo("##build_profile", label.c_str())) {
            ImGui::TextDisabled("编译配置");
            ImGui::BeginDisabled(settings.gppPath.empty() || settings.useCmdTemplate);
            if (ImGui::Selectable("Release  /  -O2", release)) {
                settings.dbgFlag = false; settings.optLevel = 1; g_app.SaveSettings();
            }
            if (ImGui::Selectable("Debug  /  -g -O0", debug)) {
                settings.dbgFlag = true; settings.optLevel = 2; g_app.SaveSettings();
            }
            ImGui::EndDisabled();
            ImGui::Separator();
            if (ImGui::Selectable("编译器与更多选项…")) g_app.showSettings = true;
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s\n%s", settings.gppPath.empty() ? "尚未配置编译器" : settings.gppPath.c_str(),
                              settings.useCmdTemplate ? "正在使用自定义编译命令" : "选择优化配置；保留附加参数，* 表示有附加参数。Debug 配置包含调试信息");
        ImGui::EndDisabled();

        ImGui::SameLine(0,12);
        auto action = [&](const char* id, Icon icon, const char* text, const char* actionName, bool primary=false) {
            std::string caption = narrow ? std::string() : std::string(text) + "  " + HotkeyText(actionName);
            if(primary && narrow) caption = text;
            float width = caption.empty() ? 28 : ImGui::CalcTextSize(caption.c_str()).x + 38;
            ImGui::PushStyleColor(ImGuiCol_Button, primary ? ImVec4(0.16f,0.43f,0.32f,1) : ImVec4(0,0,0,0));
            if(primary) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.93f,1,0.96f,1));
            bool clicked=ImGui::Button(id,ImVec2(width,28));
            ImVec2 pos=ImGui::GetItemRectMin();
            toolbar::DrawIcon(icon,ImVec2(pos.x+6,pos.y+6),ImGui::GetColorU32(ImGuiCol_Text));
            if(!caption.empty()) ImGui::GetWindowDrawList()->AddText(ImVec2(pos.x+29,pos.y+(28-ImGui::GetFontSize())/2),ImGui::GetColorU32(ImGuiCol_Text),caption.c_str());
            if(primary) ImGui::PopStyleColor();
            ImGui::PopStyleColor();
            if(ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s",tip(text,actionName).c_str());
            ImGui::SameLine(0,4); return clicked;
        };
        ImGui::BeginDisabled(busy || !ed);
        if(action("##compile",Icon::Build,"编译","compile")) { g_app.showConsole=true; g_app.StartCompileRun(BuildAction::Compile); ImGui::SetWindowFocus("输出"); }
        if(action("##run",Icon::Run,"编译运行","run",true)) { g_app.showConsole=true; g_app.StartCompileRun(); }
        if(action("##runOnly",Icon::Run,"运行","runOnly")) g_app.StartCompileRun(BuildAction::Run);
        if(toolbar::Button("##debug",Icon::Debug,tip("开始调试","debug").c_str())) { g_app.showDebug=true; g_app.StartCompileRun(BuildAction::Debug); }
        ImGui::EndDisabled();
        ImGui::BeginDisabled(!busy);
        if(toolbar::Button("##stop",Icon::Stop,"停止当前任务")) { g_app.killRequested.store(true); g_app.debugger.Stop(); }
        ImGui::EndDisabled();
    }
    ImGui::End(); ImGui::PopStyleVar(3);
}

static void DrawActivityBar() {
    auto* vp = ImGui::GetMainViewport();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(4,8));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,ImVec2(0,6));
    ImGui::PushStyleColor(ImGuiCol_WindowBg,ImGui::GetStyleColorVec4(ImGuiCol_MenuBarBg));
    if(ImGui::BeginViewportSideBar("##activity_bar",vp,ImGuiDir_Left,38,
       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar)) {
        auto item=[&](const char* id,toolbar::Icon icon,const char* tip,bool* visible,const char* window) {
            auto* target=ImGui::FindWindowByName(window);
            bool selected=*visible && (!target || !target->DockNode || target->DockNode->VisibleWindow==target);
            ImGui::PushStyleColor(ImGuiCol_Button,selected ? ImGui::GetStyleColorVec4(ImGuiCol_Header) : ImVec4(0,0,0,0));
            bool clicked=ImGui::Button(id,ImVec2(30,32)); ImGui::PopStyleColor();
            ImVec2 pos=ImGui::GetItemRectMin();
            toolbar::DrawIcon(icon,ImVec2(pos.x+7,pos.y+8),ImGui::GetColorU32(selected ? ImGuiCol_Text : ImGuiCol_TextDisabled));
            if(selected) ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(pos.x-4,pos.y+6),ImVec2(pos.x-2,pos.y+26),ImGui::GetColorU32(ImGuiCol_CheckMark),1);
            if(ImGui::IsItemHovered()) ImGui::SetTooltip("%s",tip);
            if(clicked) {
                if(*visible && target && target->DockNode && target->DockNode->VisibleWindow!=target) ImGui::SetWindowFocus(window);
                else { *visible=!*visible; if(*visible) ImGui::SetWindowFocus(window); }
            }
        };
        item("##files",toolbar::Icon::Files,"文件",&g_app.showFileTree,"文件");
        item("##search",toolbar::Icon::Search,"全局查找",&g_app.showSearch,"全局查找");
        item("##debugPanel",toolbar::Icon::Debug,"调试",&g_app.showDebug,"调试");
        item("##problems",toolbar::Icon::Build,"问题",&g_app.showProblems,"问题");
        item("##terminal",toolbar::Icon::Output,"终端",&g_app.showTerminal,"终端");
        item("##output",toolbar::Icon::Backup,"构建输出",&g_app.showConsole,"输出");
        ImGui::SetCursorPosY(std::max(ImGui::GetCursorPosY(),ImGui::GetWindowHeight()-86));
        item("##settings",toolbar::Icon::Settings,"设置",&g_app.showSettings,"设置");
    }
    ImGui::End(); ImGui::PopStyleColor(); ImGui::PopStyleVar(2);
}

static void DrawWorkspaceDock() {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGuiID id = ImHashStr("MiniCPP.Workspace.v2");
    bool rebuild = !ImGui::DockBuilderGetNode(id) || g_resetWorkspace;
    ImGui::DockSpaceOverViewport(id, vp, ImGuiDockNodeFlags_PassthruCentralNode);
    if (!rebuild) {
        auto* file=ImGui::FindWindowByName("文件"); auto* output=ImGui::FindWindowByName("输出");
        auto* root=ImGui::DockBuilderGetNode(id);
        // Plugins choose a default tool-window dock without exposing host objects.
        if(file && output && root) g_app.plugins.SetDockTargets(file->DockId,output->DockId,root->ID);
        return;
    }
    g_resetWorkspace = false;
    ImGui::DockBuilderRemoveNode(id);
    ImGui::DockBuilderAddNode(id, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodePos(id, vp->WorkPos);
    ImGui::DockBuilderSetNodeSize(id, vp->WorkSize);
    ImGuiID center = id, left, bottom, right;
    ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, std::min(0.30f, 230.0f / vp->WorkSize.x), &left, &center);
    ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, std::min(0.30f, 210.0f / vp->WorkSize.y), &bottom, &center);
    ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.29f, &right, &center);
    ImGui::DockBuilderDockWindow("文件", left);
    ImGui::DockBuilderDockWindow("全局查找", left);
    ImGui::DockBuilderDockWindow("编辑器", center);
    ImGui::DockBuilderDockWindow("问题", bottom);
    ImGui::DockBuilderDockWindow("输出", bottom);
    ImGui::DockBuilderDockWindow("终端", bottom);
    ImGui::DockBuilderDockWindow("调试", bottom);
    ImGui::DockBuilderDockWindow("时钟", bottom);
    ImGui::DockBuilderDockWindow("示例插件 Hello", bottom);
    g_app.plugins.SetDockTargets(left,bottom,right);
    if (auto* node = ImGui::DockBuilderGetNode(center)) node->LocalFlags |= ImGuiDockNodeFlags_AutoHideTabBar;
    // Marketplace/settings are occasional tasks; do not permanently split editor space for them.
    ImGui::DockBuilderFinish(id);
}

// ---------------- Editor tabs ----------------
static void DrawEditorWindow() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    if (!ImGui::Begin("编辑器", nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        ImGui::End(); ImGui::PopStyleVar(); return;
    }
    static uint64_t lastActive = 0;
    uint64_t requested = g_app.ActiveEditor() ? g_app.ActiveEditor()->m_uid : 0;
    const bool requestSelection = requested && requested != lastActive;
    int action = 0, target = -1;
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(14, 8));
    if (ImGui::BeginTabBar("##files", ImGuiTabBarFlags_FittingPolicyScroll | ImGuiTabBarFlags_TabListPopupButton)) {
        for (int i = 0; i < (int)g_app.editors.size(); ++i) {
            auto& ed = g_app.editors[i];
            std::string label = ed->m_name + "###file_" + std::to_string(ed->m_uid);
            bool open = true;
            ImGuiTabItemFlags flags = ed->m_dirty ? ImGuiTabItemFlags_UnsavedDocument : 0;
            if (ed->m_uid == requested && requestSelection) flags |= ImGuiTabItemFlags_SetSelected;
            bool selected = ImGui::BeginTabItem(label.c_str(), &open, flags);
            if (selected) {
                if ((!requestSelection || ed->m_uid == requested) && g_app.active != i) { g_app.active = i; g_app.SyncDiagPanel(); }
                ImGui::EndTabItem();
            }
            if (ImGui::IsItemHovered() && ed->HasPath()) ImGui::SetTooltip("%s", utils::WToU8(ed->m_path).c_str());
            if (!open) { action = 1; target = i; }
            if (ImGui::BeginPopupContextItem()) {
                if (ImGui::MenuItem("关闭")) { action = 1; target = i; }
                if (ImGui::MenuItem("关闭其他")) { action = 2; target = i; }
                if (ImGui::MenuItem("全部关闭")) { action = 3; target = i; }
                ImGui::Separator();
                if (ed->HasPath()) {
                    if (ImGui::MenuItem("复制文件路径")) ImGui::SetClipboardText(utils::WToU8(ed->m_path).c_str());
                    if (ImGui::MenuItem("在资源管理器中显示")) {
                        std::wstring args = L"/select,\"" + ed->m_path + L"\"";
                        ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
                    }
                }
                ImGui::EndPopup();
            }
        }
        if (ImGui::TabItemButton("+", ImGuiTabItemFlags_Trailing)) action = 4;

        ImGui::EndTabBar();
    }
    ImGui::PopStyleVar();
    // Apply close requests after drawing: no dangling references or skipped tabs.
    if (action == 1) g_app.CloseEditor(target);
    else if (action == 2 || action == 3) {
        for (int i = (int)g_app.editors.size() - 1; i >= 0; --i) {
            if (action == 2 && i == target) continue;
            size_t before = g_app.editors.size();
            g_app.CloseEditor(i);
            if (g_app.editors.size() == before) break;
        }
    } else if (action == 4) g_app.NewFile();
    lastActive = requested;
    CodeEditor* ed = g_app.ActiveEditor();
    if (ed) {
        ImGui::Indent(14);
        ImGui::TextDisabled("%s", ed->HasPath() ? utils::WToU8(utils::FileBase(utils::DirName(ed->m_path))).c_str() : "工作区");
        ImGui::SameLine(); ImGui::TextDisabled(" / "); ImGui::SameLine();
        ImGui::TextUnformatted(ed->m_name.c_str());
        ImGui::Unindent(14);
        ImGui::Separator();
        ed->Draw();
    } else {
        ImVec2 available = ImGui::GetContentRegionAvail();
        ImGui::SetCursorPos(ImVec2(std::max(24.0f, available.x * 0.5f - 145), std::max(50.0f, available.y * 0.32f)));
        ImGui::BeginGroup();
        ImGui::TextColored(ImGui::GetStyleColorVec4(ImGuiCol_CheckMark), "ccrIDE");
        ImGui::Spacing(); ImGui::TextUnformatted("开始编写你的 C++ 程序");
        ImGui::Spacing(); ImGui::TextDisabled("新建文件，或打开已有代码继续编写。");
        ImGui::Spacing(); ImGui::Spacing();
        if (ImGui::Button("新建文件", ImVec2(124, 36))) g_app.NewFile();
        ImGui::SameLine();
        if (ImGui::Button("打开文件", ImVec2(124, 36))) g_app.OpenFileDialog();
        ImGui::EndGroup();
    }
    ImGui::End(); ImGui::PopStyleVar();
}

// ---------------- Status bar ----------------
static void DrawStatusBar() {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14, 4));
    if (ImGui::BeginViewportSideBar("##statusbar", vp, ImGuiDir_Down, 27,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings)) {
        CodeEditor* ed = g_app.ActiveEditor();
        const bool busy = g_app.procBusy.load();
        std::string status = busy ? g_app.GetProcStatus() : g_app.diagStatus.empty() ? "就绪" : g_app.diagStatus;
        int errors = 0, warnings = 0;
        for (auto& d : g_app.activeDiags) { if (d.isErr) ++errors; else ++warnings; }
        if (errors) ImGui::TextColored(ImVec4(0.95f,0.44f,0.43f,1), "%d 个错误", errors);
        else if (warnings) ImGui::TextColored(ImVec4(0.90f,0.69f,0.32f,1), "%d 个警告", warnings);
        else ImGui::TextDisabled("%s", status.c_str());
        if (ed) {
            float right = 230;
            if (vp->Size.x > 800 && ed->HasPath() && ImGui::CalcTextSize(ed->m_name.c_str()).x < vp->Size.x - 450) {
                ImGui::SameLine(0, 22);
                ImGui::TextDisabled("%s", ed->m_name.c_str());
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", utils::WToU8(ed->m_path).c_str());
            }
            ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 12, vp->Size.x - right));
            ImGui::TextDisabled("行 %d : 列 %d    |    %s    C++", (int)ed->LineOf(ed->m_cursor) + 1,
                               (int)ed->VisualCol(ed->m_cursor) + 1, ed->m_enc == 1 ? "GBK" : "UTF-8");
        }
    }
    ImGui::End(); ImGui::PopStyleVar();
}

static void DrawEditorAppearanceSettings() {
    auto& settings = g_app.settings;
    ImGui::SeparatorText("编辑器配色与辅助线");
    ImGui::Checkbox("彩虹括号", &settings.rainbowBrackets);
    ImGui::Checkbox("缩进提示线", &settings.showIndentGuides);
    ImGui::Checkbox("彩虹缩进线", &settings.rainbowIndentLines);
    ImGui::Checkbox("彩虹填充缩进", &settings.rainbowIndentFill);
    ImGui::Checkbox("显示空格与 Tab 标记", &settings.showWhitespace);
    static int selected = (int)EditorColor::Selection;
    const auto& spec = EditorColorSpecs[selected];
    std::string name = std::string(spec.group) + " / " + spec.label;
    ImGui::TextUnformatted("颜色项目");
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo("##editor_color_role", name.c_str(), ImGuiComboFlags_HeightLarge)) {
        for (const char* group : {"编辑器", "语法", "语法检查"}) {
            ImGui::SeparatorText(group);
            for (int i=0; i<(int)EditorColor::Count; ++i) {
                const auto& item=EditorColorSpecs[i];
                if (strcmp(group,item.group)) continue;
                ImGui::PushID(i);
                if (ImGui::Selectable(item.label, selected==i)) selected=i;
                ImGui::PopID();
            }
        }
        ImGui::EndCombo();
    }
    std::string key = settings.theme + "/" + EditorColorSpecs[selected].key;
    ImVec4 color = ImGui::ColorConvertU32ToFloat4(g_app.editorPalette[selected]);
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::ColorEdit4("##editor_color", &color.x, ImGuiColorEditFlags_DisplayHex | ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreviewHalf)) {
        settings.editorColors[key] = ImGui::ColorConvertFloat4ToU32(color);
        g_app.ApplyTheme();
    }
    if (ImGui::Button("恢复此项默认色")) { settings.editorColors.erase(key); g_app.ApplyTheme(); }
    if (ImGui::GetWindowWidth() > 440) ImGui::SameLine();
    if (ImGui::Button("恢复当前主题全部颜色")) {
        std::string prefix = settings.theme + "/";
        for (auto it=settings.editorColors.begin(); it!=settings.editorColors.end(); )
            if (it->first.rfind(prefix, 0) == 0) it = settings.editorColors.erase(it); else ++it;
        g_app.ApplyTheme();
    }
    ImGui::TextWrapped("颜色实时生效；深色和浅色分别保存。红杠表示相对上次保存新增或修改的行，绿杠表示原有或已保存行。Ctrl+点击行号设置断点。");
    ImGui::PushStyleColor(ImGuiCol_ChildBg, g_app.EditorColorValue(EditorColor::Background));
    ImGui::BeginChild("##palette_preview", ImVec2(0,64), ImGuiChildFlags_Borders);
    ImGui::TextColored(color, "Aa  示例文字  012345  { [ ( ) ] }");
    ImGui::PushStyleColor(ImGuiCol_Text, g_app.EditorColorValue(EditorColor::SelectedText));
    ImVec2 start=ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddRectFilled(start, ImVec2(start.x+180,start.y+20), g_app.EditorColorValue(EditorColor::Selection));
    ImGui::TextUnformatted("选中文字预览");
    ImGui::PopStyleColor(); ImGui::EndChild(); ImGui::PopStyleColor();
}

// ---------------- 设置窗口 ----------------
static void DrawSettingsWindow() {
    ImVec2 screen = ImGui::GetMainViewport()->WorkSize;
    ImGui::SetNextWindowSize(ImVec2(std::min(850.0f, std::max(340.0f, screen.x - 32)),
                                  std::min(720.0f, std::max(280.0f, screen.y - 32))), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(340, 280), ImVec2(FLT_MAX, FLT_MAX));
    if (!ImGui::Begin("设置", &g_app.showSettings, ImGuiWindowFlags_NoScrollbar)) {
        g_hkRecording = false;   // 窗口关闭时取消录制, 避免全局快捷键一直失效
        g_hkRecAct.clear();
        ImGui::End();
        return;
    }

    auto field = [](const char* label) {
        ImGui::TextUnformatted(label);
        ImGui::SetNextItemWidth(-FLT_MIN);
    };
    auto help = [](const char* text) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("%s", text);
        ImGui::PopStyleColor();
    };
    static int section=0;
    const char* sections[]={"编译器与构建","代码辅助","运行与调试","外观与颜色","插件","快捷键"};
    float bodyHeight=ImGui::GetContentRegionAvail().y-ImGui::GetFrameHeightWithSpacing()-12;
    if(ImGui::GetWindowWidth()>=540) {
        ImGui::BeginChild("##settings_categories",ImVec2(142,bodyHeight));
        ImGui::TextDisabled("偏好设置"); ImGui::Spacing();
        for(int i=0;i<6;++i) if(ImGui::Selectable(sections[i],section==i,0,ImVec2(0,30))) {
            section=i; g_hkRecAct.clear(); g_hkRecording=false;
        }
        ImGui::EndChild(); ImGui::SameLine(0,16);
    } else {
        ImGui::SetNextItemWidth(-FLT_MIN);
        if(ImGui::Combo("##settings_section",&section,sections,6)) { g_hkRecAct.clear(); g_hkRecording=false; }
        bodyHeight-=ImGui::GetFrameHeightWithSpacing();
    }
    ImGui::BeginChild("##settings_body", ImVec2(0,bodyHeight));
    ImGui::PushID(section);
    if(section==0) {
    ImGui::SeparatorText("编译器");
    field("g++ 路径");
    ui::InputTextStd("##g++ 路径", g_app.settings.gppPath);
    field("附加编译参数");
    ui::InputTextStd("##附加编译参数", g_app.settings.extraFlags);
    help("空格分隔, 如: -g -Wall -std=c++20 (默认 -std=c++17 -O2)");
    if (ImGui::Button("递归查找编译器")) g_app.StartCompilerSearch();
    if(!g_app.compilerCandidates.empty()) {
        ImGui::SameLine();
        if(ImGui::Button("选择已找到的编译器"))g_app.compilerPickerOpen=true;
    }
    if(g_app.compilerScanBusy.load())ImGui::TextWrapped("正在后台扫描本机磁盘，可在编译器选择窗口取消。");
    else if(!g_app.compilerScanMessage.empty())ImGui::TextWrapped("%s",g_app.compilerScanMessage.c_str());


    }
    if(section==1) {
    ImGui::SeparatorText("代码辅助");
    if (ImGui::Checkbox("代码补全与参数提示", &g_app.settings.codeCompletion)) {
        if (!g_app.settings.codeCompletion) for (auto& ed : g_app.editors) {
            ed->CloseCompletion(); ed->CloseCalltip();
        }
    }
    ImGui::Checkbox("语法高亮", &g_app.settings.syntaxHighlight);
    if (ImGui::Checkbox("自动语法检查", &g_app.settings.autoCheck)) {
        if (g_app.settings.autoCheck) g_app.MarkDiag();
    }
    help("三个开关相互独立。关闭补全不影响高亮或检查；关闭高亮仍可导航、折叠和匹配括号。自动检查在输入停顿后运行，关闭后已启动的检查可完成，但不再应用其结果；编译错误仍会显示。");

    }
    if(section==0) {
    ImGui::SeparatorText("编译与运行 (F10 / F11 / F12)");
    int modeIdx = g_app.settings.useCmdTemplate ? 1 : 0;
    field("编译方式");
    if (ImGui::Combo("##编译方式", &modeIdx, "标准选项 (勾选下方开关)\0自定义命令行模板\0")) {
        g_app.settings.useCmdTemplate = (modeIdx == 1);
    }
    if (!g_app.settings.useCmdTemplate) {
        const char* stds[] = { "C++17", "C++20", "C++14", "C++11", "C++23" };
        int sv = g_app.settings.stdVersion;
        if (sv < 0 || sv > 4) sv = 0;
        field("C++ 标准");
    if (ImGui::Combo("##C++ 标准", &sv, stds, 5)) g_app.settings.stdVersion = sv;
        const char* opts[] = { "无", "-O2", "-O0", "-O1", "-O3", "-Os" };
        int ol = g_app.settings.optLevel;
        if (ol < 0 || ol > 5) ol = 1;
        field("优化级别");
    if (ImGui::Combo("##优化级别", &ol, opts, 6)) g_app.settings.optLevel = ol;
        ImGui::Checkbox("警告 -Wall -Wextra", &g_app.settings.warnAll);
        if (ImGui::GetWindowWidth() > 420) ImGui::SameLine();
        ImGui::Checkbox("调试信息 -g", &g_app.settings.dbgFlag);
        help("实际命令: <g++ 路径> <源文件> <上面的选项> <附加编译参数> -o <exe>");
    } else {
        ImGui::TextWrapped("占位符: {gpp} 编译器  {file} 源文件  {exe} 输出程序  {dir} 所在目录  "
                           "{std}/{opt}/{warn}/{dbg} 上方开关  {extra} 附加编译参数");
        field("命令行模板");
        ui::InputTextMultilineStd("##命令行模板", g_app.settings.commandTemplate, ImVec2(-FLT_MIN, 96));
        if (ImGui::Button("填入默认模板")) {
            g_app.settings.commandTemplate = "{gpp} {std} {opt} {warn} {dbg} {extra} {file} -o {exe}";
        }
        if (ImGui::GetWindowWidth() > 420) ImGui::SameLine();
        if (ImGui::Button("清空 (留空=用默认)")) g_app.settings.commandTemplate.clear();
        help("首段必须是可执行文件; 路径含空格请用引号包住, 如 \"{gpp}\" 或 -I\"D:/my lib\"。");
    }

    }
    if(section==2) {
    ImGui::SeparatorText("运行与资源限制");
    field("运行时间上限 (毫秒)");
    ImGui::InputInt("##runTime", &g_app.settings.runTimeLimitMs);
    if (ImGui::IsItemDeactivatedAfterEdit()) g_app.settings.runTimeLimitMs = std::clamp(g_app.settings.runTimeLimitMs, 100, 3600000);
    field("运行内存上限 (MiB，0 表示不限制)");
    ImGui::InputInt("##runMemory", &g_app.settings.runMemoryLimitMB);
    if (ImGui::IsItemDeactivatedAfterEdit()) g_app.settings.runMemoryLimitMB = std::clamp(g_app.settings.runMemoryLimitMB, 0, 16384);
    field("总输出上限 (MiB，stdout 与 stderr 合计)");
    ImGui::InputInt("##runOutput", &g_app.settings.runOutputLimitMB);
    if (ImGui::IsItemDeactivatedAfterEdit()) g_app.settings.runOutputLimitMB = std::clamp(g_app.settings.runOutputLimitMB, 1, 64);
    field("编译时间上限 (毫秒)");
    ImGui::InputInt("##compileTime", &g_app.settings.compileTimeLimitMs);
    if (ImGui::IsItemDeactivatedAfterEdit()) g_app.settings.compileTimeLimitMs = std::clamp(g_app.settings.compileTimeLimitMs, 100, 3600000);
    field("程序参数");
    ui::InputTextStd("##runArguments", g_app.settings.runArguments);
    help("按 Windows 参数规则解析，含空格的参数用双引号包住，空参数写作双引号对。");
    field("运行目录 (留空使用源文件目录)");
    ui::InputTextStd("##runDirectory", g_app.settings.runDirectory);
    field("GDB 路径 (留空使用编译器目录中的 gdb.exe)");
    ui::InputTextStd("##gdbPath",g_app.settings.gdbPath);
    field("调试标准输入文件 (留空发送 EOF)");
    ui::InputTextStd("##debugInputFile",g_app.settings.debugInputFile);
    field("环境变量覆盖 (每行 NAME=value)");
    ui::InputTextMultilineStd("##runEnvironment", g_app.settings.runEnvironment, ImVec2(-FLT_MIN, 78));
    help("环境覆盖只用于目标程序，值会随设置保存在本机。Windows 进程组限制提交内存并清理后代，每个任务最多 128 个进程；这是资源管理，不能作为安全沙箱。输出面板最多保留 8 MiB，满载时可能省略显示；编译日志总输出上限为 16 MiB。");

    }
    if(section==3) {
    ImGui::SeparatorText("外观");
    const char* themes[] = { "深色", "浅色" };
    int themeIdx = (g_app.settings.theme == "light") ? 1 : 0;
    field("主题");
    if (ImGui::Combo("##主题", &themeIdx, themes, 2)) {
        g_app.settings.theme = themeIdx ? "light" : "dark";
        g_app.ApplyTheme();
    }
    field("编辑器字号");
    if (ImGui::SliderInt("##编辑器字号", &g_app.settings.fontSize, 12, 28)) {
        g_app.settings.fontSize = std::max(12, std::min(28, g_app.settings.fontSize));
        g_app.fontRebuildPending = true;   // 字体下一帧重建, 即时生效
    }

    DrawEditorAppearanceSettings();

    }
    if(section==4) {
    ImGui::SeparatorText("插件市场");
    field("线上市场地址");
    ui::InputTextStd("##线上市场地址", g_app.settings.marketUrl);
    help("留空则仅显示随附插件; 填写后到插件市场点 [刷新线上市场]");

    }
    if(section==5) {
    // ---- 快捷键配置 ----
    ImGui::SeparatorText("快捷键");
    help("点击当前组合后按下新的快捷键，Esc 取消。");
    ImGui::Columns(2, "hkedit", false);
    for (auto& d : g_hkDefs) {
        bool isRec = (g_hkRecAct == d.action);
        ImGui::TextUnformatted(d.label);
        ImGui::NextColumn();
        if (isRec) {
            ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.4f, 1.0f), "按新组合键...");
        } else {
            std::string combo = HotkeyText(d.action);
            char buf[96];
            snprintf(buf, sizeof buf, "%s##hk", combo.c_str());
            ImGui::PushID(d.action);
            if (ImGui::Button(buf, ImVec2(std::min(200.0f, ImGui::GetContentRegionAvail().x), 0))) g_hkRecAct = d.action;
            ImGui::PopID();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("点击后按下新的快捷键组合");
        }
        ImGui::NextColumn();
    }
    ImGui::Columns(1);

    // 录制逻辑: 捕获按键组合
    if (!g_hkRecAct.empty()) {
        g_hkRecording = true;   // 录制期间不触发全局快捷键 (否则会把正在按的组合执行掉)
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.85f, 0.4f, 1.0f));
        ImGui::TextWrapped("正在录制…请按下新的快捷键 (Esc 取消)");
        ImGui::PopStyleColor();
        ImGuiIO& io = ImGui::GetIO();
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            g_hkRecAct.clear();
        } else {
            for (int ki = 0; ki < (int)ImGuiKey_NamedKey_COUNT; ki++) {
                ImGuiKey k = (ImGuiKey)(ImGuiKey_NamedKey_BEGIN + ki);
                if (!ImGui::IsKeyPressed(k)) continue;
                const char* tok = KeyToken(k);
                if (!tok) continue;
                std::string combo;
                if (io.KeyCtrl)  combo += "Ctrl+";
                if (io.KeyShift) combo += "Shift+";
                if (io.KeyAlt)   combo += "Alt+";
                combo += tok;
                SetHotkey(g_hkRecAct.c_str(), combo);
                g_hkRecAct.clear();
                break;
            }
        }
    } else {
        g_hkRecording = false;
    }

    }
    ImGui::PopID();
    ImGui::EndChild();
    ImGui::Separator();
    if (ImGui::Button("保存设置", ImVec2(120, 0))) g_app.SaveSettings();
    ImGui::SameLine();
    if (ImGui::Button("关闭", ImVec2(120, 0))) g_app.showSettings = false;
    ImGui::End();
}

static void DrawProblemsWindow() {
    if(!ImGui::Begin("问题",&g_app.showProblems)) { ImGui::End(); return; }
    size_t errors=0; for(auto& d:g_app.activeDiags) if(d.isErr) ++errors;
    ImGui::TextDisabled("当前文件  ·  %zu 个错误   %zu 个警告",errors,g_app.activeDiags.size()-errors);
    ImGui::Separator();
    if(g_app.activeDiags.empty()) ImGui::TextDisabled("当前没有诊断问题");
    if(ImGui::BeginChild("##problemList",ImVec2(0,0),ImGuiChildFlags_None,ImGuiWindowFlags_HorizontalScrollbar)) {
        for(size_t i=0;i<g_app.activeDiags.size();++i) {
            auto& d=g_app.activeDiags[i]; ImGui::PushID((int)i);
            ImGui::PushStyleColor(ImGuiCol_Text,d.isErr ? ImVec4(0.96f,0.40f,0.43f,1) : ImVec4(0.87f,0.69f,0.29f,1));
            std::string label=std::string(d.isErr ? "错误" : "警告") + "  ·  " + std::to_string(d.line) + " 行    " + d.msg;
            if(ImGui::Selectable(label.c_str())) g_app.GotoEditorLine(d.line);
            ImGui::PopStyleColor(); ImGui::PopID();
        }
    }
    ImGui::EndChild(); ImGui::End();
}

static void DrawDebuggerWindow() {
    if(!ImGui::Begin("调试",&g_app.showDebug)) { ImGui::End(); return; }
    auto state=g_app.debugger.Snapshot();
    ImGui::BeginDisabled(g_app.procBusy.load()||state.active);
    if(ImGui::Button("开始 (F5)")) g_app.StartCompileRun(BuildAction::Debug);
    ImGui::EndDisabled(); ImGui::SameLine();
    ImGui::BeginDisabled(!state.stopped);
    if(ImGui::Button("继续")) g_app.debugger.Continue();
    ImGui::SameLine(); if(ImGui::Button("步过")) g_app.debugger.Step();
    ImGui::SameLine(); if(ImGui::Button("步入")) g_app.debugger.Step(true);
    ImGui::EndDisabled(); ImGui::SameLine();
    ImGui::BeginDisabled(!state.active);
    if(ImGui::Button("暂停")) g_app.debugger.Pause();
    ImGui::SameLine(); if(ImGui::Button("停止")) g_app.debugger.Stop();
    ImGui::EndDisabled();
    ImGui::SameLine(); ImGui::TextDisabled("(?)");
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Ctrl+点击行号设置断点；没有断点时暂停在 main。\n调试使用 -g -O0；设置中可指定输入文件，留空为 EOF。");
    if(!state.status.empty()) ImGui::TextWrapped("%s",state.status.c_str());
    ImGui::Separator();
    auto variables=[&]() {
        if(state.variables.empty()) ImGui::TextDisabled("暂停后显示局部变量");
        if(ImGui::BeginTable("##vars",2,ImGuiTableFlags_RowBg|ImGuiTableFlags_Resizable|ImGuiTableFlags_ScrollY,ImVec2(0,0))) {
            ImGui::TableSetupColumn("名称"); ImGui::TableSetupColumn("值"); ImGui::TableHeadersRow();
            for(const auto& v:state.variables) { ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextUnformatted(v.first.c_str()); ImGui::TableNextColumn(); ImGui::TextUnformatted(v.second.c_str()); }
            ImGui::EndTable();
        }
    };
    auto stack=[&]() {
        for(const auto& f:state.frames) {
            ImGui::PushID(f.level);
            std::string label="#"+std::to_string(f.level)+"  "+f.function+" : "+std::to_string(f.line);
            if(ImGui::Selectable(label.c_str())) { g_app.debugger.SelectFrame(f.level); if(!f.file.empty()&&f.line>0) g_app.OpenAtFile(f.file,f.line); }
            ImGui::PopID();
        }
        if(state.frames.empty()) ImGui::TextDisabled("暂停后显示调用栈");
    };
    if(ImGui::BeginTabBar("##debugViews")) {
        if(ImGui::BeginTabItem("检查")) {
            static std::string expression;
            ImGui::BeginDisabled(!state.stopped); ImGui::SetNextItemWidth(-66);
            bool evaluate=ui::InputTextStd("##debugExpression",expression,ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SameLine(); evaluate=ImGui::Button("求值")||evaluate;
            if(evaluate&&!expression.empty()) g_app.debugger.Evaluate(expression);
            ImGui::EndDisabled();
            if(!state.evaluation.empty()) ImGui::TextWrapped("结果: %s",state.evaluation.c_str());
            if(ImGui::BeginTable("##inspect",2,ImGuiTableFlags_Resizable|ImGuiTableFlags_BordersInnerV)) {
                ImGui::TableNextRow(); ImGui::TableNextColumn();
                ImGui::TextDisabled("局部变量"); ImGui::BeginChild("##debugVariables",ImVec2(0,0)); variables(); ImGui::EndChild();
                ImGui::TableNextColumn(); ImGui::TextDisabled("调用栈"); ImGui::BeginChild("##debugStack",ImVec2(0,0)); stack(); ImGui::EndChild();
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }
        if(ImGui::BeginTabItem("断点")) {
            ImGui::BeginChild("##breakpoints",ImVec2(0,0));
            for(auto& editor:g_app.editors) { auto points=editor->m_breakpoints;
                for(int line:points) {
                    ImGui::PushID((int)editor->m_uid); ImGui::PushID(line);
                    if(ImGui::SmallButton("移除")) editor->ToggleBreakpoint(line);
                    ImGui::SameLine(); if(ImGui::Selectable((editor->m_name+":"+std::to_string(line)).c_str())) g_app.OpenAtFile(utils::WToU8(editor->m_path),line);
                    ImGui::PopID(); ImGui::PopID();
                }
            }
            ImGui::EndChild(); ImGui::EndTabItem();
        }
        if(ImGui::BeginTabItem("调试日志")) {
            if(ImGui::SmallButton("复制日志")) ImGui::SetClipboardText(state.log.c_str());
            ImGui::BeginChild("##debugLog",ImVec2(0,0),ImGuiChildFlags_None,ImGuiWindowFlags_HorizontalScrollbar);
            ImGui::TextUnformatted(state.log.c_str()); ImGui::EndChild(); ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

// ---------------- 关于 ----------------
static void DrawAboutWindow() {
    if (!ImGui::Begin("关于", &g_app.showAbout, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return;
    }
    ImGui::Text("ccrIDE v%s",CCRIDE_VERSION);
    ImGui::Text("构建时间: %s",CCRIDE_BUILD_TIME);
    ImGui::TextUnformatted("轻量 C++ IDE，支持编译、运行与 GDB 调试");
    ImGui::TextUnformatted("技术栈: Dear ImGui + GLFW + OpenGL + WinINet");
    ImGui::TextWrapped("数据目录: %s", utils::WToU8(g_app.dataDir).c_str());
    ImGui::TextWrapped("部署方式: %s", g_app.portableMode ? "便携模式，设置跟随程序目录" : "本机模式");
    ImGui::Separator();
    ImGui::TextDisabled("全局快捷键可在 设置 -> 快捷键 中自定义");
    ImGui::TextDisabled("查找 Ctrl+F | 补全 Ctrl+Space | 书签 F2 | 跳转定义 Ctrl+B");
    if (ImGui::Button("关闭")) g_app.showAbout = false;
    ImGui::End();
}

// ---------------- 全局查找 ----------------
namespace fs = std::filesystem;
static void DrawSearchWindow() {
    struct Hit { std::string file; int line; std::string text; };
    static std::string dir, kw;
    static bool cs = false, wholeWord = false;
    static std::vector<Hit> hits;
    static bool truncated = false, searched = false;
    static std::string status;

    if (!ImGui::Begin("全局查找", &g_app.showSearch)) { ImGui::End(); return; }

    bool doSearch = false;
    ui::InputTextStd("目录", dir);
    ImGui::SameLine();
    if (ImGui::Button("浏览##dir")) {
        std::wstring p;
        if (utils::OpenFileDialog(g_app.dataDir, p)) dir = utils::WToU8(utils::DirName(p));
    }
    if (dir.empty()) {
        // 默认搜索当前打开文件的目录
        if (CodeEditor* ed = g_app.ActiveEditor())
            if (ed->HasPath()) dir = utils::WToU8(utils::DirName(ed->m_path));
        ImGui::TextDisabled("目录将默认使用当前文件的所在目录");
    }
    ui::InputTextStd("搜索内容", kw);
    ImGui::Checkbox("区分大小写", &cs);
    ImGui::SameLine();
    ImGui::Checkbox("整词匹配", &wholeWord);
    ImGui::SameLine();
    if (ImGui::Button("搜索")) doSearch = true;
    ImGui::SameLine();
    if (ImGui::Button("清空结果")) { hits.clear(); searched = false; status.clear(); }
    ImGui::TextDisabled("递归文本文件(≤2MB), 自动跳过隐藏/构建目录; 最多 4000 条结果");

    if (doSearch) {
        hits.clear();
        truncated = false;
        searched = true;
        status.clear();
        std::wstring wdir = utils::U8ToW(dir);
        if (!kw.empty() && GetFileAttributesW(wdir.c_str()) != INVALID_FILE_ATTRIBUTES) {
            auto lower = [](std::string s) {
                for (auto& c : s) c = (char)tolower((unsigned char)c);
                return s;
            };
            std::string needle = cs ? kw : lower(kw);
            auto textFile = [](const std::wstring& fn) {
                const wchar_t* exts[] = { L".cpp", L".cc", L".cxx", L".c", L".h", L".hpp", L".hh",
                                          L".txt", L".md", L".json", L".ini", L".cfg", L".log",
                                          L".xml", L".html", L".css", L".js", L".ts", L".py",
                                          L".csv", L".cmake", L".mk", L".bat", L".ps1" };
                size_t d = fn.find_last_of(L'.');
                std::wstring e = (d == std::wstring::npos) ? L"" : fn.substr(d);
                for (auto x : exts) if (_wcsicmp(e.c_str(), x) == 0) return true;
                return false;
            };
            auto match = [&](const std::string& line) {
                std::string probe = cs ? line : lower(line);
                size_t pos = 0;
                while ((pos = probe.find(needle, pos)) != std::string::npos) {
                    if (!wholeWord) return true;
                    auto isw = [](unsigned char c) { return isalnum(c) || c == '_'; };
                    bool lb = (pos == 0) || !isw((unsigned char)probe[pos - 1]);
                    size_t end = pos + needle.size();
                    bool rb = (end >= probe.size()) || !isw((unsigned char)probe[end]);
                    if (lb && rb) return true;
                    pos += needle.size();
                }
                return false;
            };

            const int MAX_HITS = 4000;
            const size_t MAX_FILE = 2ull * 1024 * 1024;
            // 忽略目录
            auto skipDir = [](const std::wstring& n) {
                const wchar_t* sk[] = { L"build", L".git", L".svn", L"node_modules", L"bin", L"obj", L".vs", L"backup" };
                for (auto s : sk) if (_wcsicmp(n.c_str(), s) == 0) return true;
                return false;
            };

            std::vector<fs::path> pending; pending.push_back(wdir);
            while (!pending.empty() && (int)hits.size() < MAX_HITS) {
                fs::path p = pending.back(); pending.pop_back();
                std::error_code ec;
                if (!fs::exists(p, ec)) continue;
                if (fs::is_directory(p, ec)) {
                    if (skipDir(p.filename().wstring())) continue;
                    if (p == g_app.backupDir || p == g_app.dataDir) continue;   // 跳过 IDE 数据目录
                    std::vector<fs::path> entries;
                    for (fs::directory_iterator it(p, fs::directory_options::skip_permission_denied, ec), end;
                         it != end; it.increment(ec)) {
                        if (ec) break;
                        entries.push_back(it->path());
                    }
                    // 目录优先, 且按名称排序保证可读性
                    std::stable_sort(entries.begin(), entries.end(), [](const fs::path& a, const fs::path& b) {
                        bool da = fs::is_directory(a), db = fs::is_directory(b);
                        if (da != db) return da;
                        return a.filename().wstring() < b.filename().wstring();
                    });
                    for (auto it = entries.rbegin(); it != entries.rend(); ++it) pending.push_back(*it);
                } else {
                    std::wstring fn = p.filename().wstring();
                    if (!textFile(fn)) continue;
                    unsigned long long sz = utils::FileSize(p.wstring());
                    if (sz == 0 || sz > MAX_FILE) continue;
                    std::string content;
                    if (!utils::ReadFileU8(p.wstring(), content)) continue;
                    if (content.find('\0') != std::string::npos) continue;   // 二进制
                    std::string fileU8 = utils::WToU8(p.wstring());
                    size_t start = 0, lineno = 1;
                    while (start <= content.size() && (int)hits.size() < MAX_HITS) {
                        size_t nl = content.find('\n', start);
                        std::string line = content.substr(start, (nl == std::string::npos ? content.size() : nl) - start);
                        if (match(line)) hits.push_back({ fileU8, (int)lineno, line });
                        if (nl == std::string::npos) break;
                        start = nl + 1;
                        lineno++;
                    }
                }
            }
            truncated = ((int)hits.size() >= MAX_HITS);
            status = "共 " + std::to_string(hits.size()) + " 处结果" + (truncated ? " (已达上限, 已截断)" : "");
        } else if (kw.empty()) {
            status = "请输入搜索内容";
        } else {
            status = "目录不存在: " + dir;
        }
    }

    if (!status.empty())
        ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1), "%s", status.c_str());

    ImGui::Separator();
    ImGui::PushFont(g_app.editorFont ? g_app.editorFont : ImGui::GetFont());
    ImVec2 avail = ImGui::GetContentRegionAvail();
    if (ImGui::BeginChild("##gsearch", avail, false,
                          ImGuiWindowFlags_HorizontalScrollbar)) {
        if (hits.empty()) {
            if (searched) ImGui::TextDisabled("没有匹配");
            else ImGui::TextDisabled("在上方输入关键字后点 [搜索]");
        }
        for (int i = 0; i < (int)hits.size(); i++) {
            auto& h = hits[i];
            // 只显示文件名 + 行号, 悬停显示完整路径
            std::string base = utils::WToU8(utils::FileBase(utils::U8ToW(h.file)));
            std::string label = "(" + std::to_string(h.line) + ")  " + base + "  |  " + h.text;
            if (label.size() > 500) label.resize(500);
            ImGui::PushID(i);
            if (ImGui::Selectable(label.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick)) {
                if (ImGui::IsMouseDoubleClicked(0) || hits.size() < 200) {   // 少量结果单击即跳
                    g_app.OpenAtFile(h.file, h.line);
                }
            }
            ImGui::PopID();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s:%d", h.file.c_str(), h.line);
        }
    }
    ImGui::EndChild();
    ImGui::PopFont();
    ImGui::End();
}

// ---------------- 文件树 ----------------
static std::wstring PickFolderW() {
    BROWSEINFOW bi{};
    bi.lpszTitle = L"选择要浏览的文件夹";
    bi.ulFlags   = BIF_RETURNONLYFSDIRS | BIF_USENEWUI;
    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    std::wstring r;
    if (pidl) {
        wchar_t buf[MAX_PATH] = { 0 };
        if (SHGetPathFromIDListW(pidl, buf)) r = buf;
        CoTaskMemFree(pidl);
    }
    return r;
}

static void DrawFileTreeWindow() {
    struct Entry { std::wstring name; bool isDir; };

    static std::wstring root;
    static std::set<std::wstring> expanded;                  // 展开的目录
    static std::map<std::wstring, std::vector<Entry>> cache; // 目录已列出的内容
    // 交互弹窗状态
    static std::wstring ctxDir;        // 目标目录 (新建/刷新)
    static std::wstring target;        // 重命名/删除目标
    static bool targetIsDir = false;
    static std::string modal;          // "", "newfile", "newdir", "rename", "del"
    static std::string nameBuf;
    static bool focusName = false;

    if (!ImGui::Begin("文件", &g_app.showFileTree)) { ImGui::End(); return; }

    auto listDir = [](const std::wstring& d, std::vector<Entry>& out) {
        std::error_code ec;
        std::vector<Entry> tmp;
        for (fs::directory_iterator it(d, fs::directory_options::skip_permission_denied, ec), end;
             it != end; it.increment(ec)) {
            if (ec) break;
            std::wstring nm = it->path().filename().wstring();
            if (!nm.empty() && nm[0] == L'.') continue;    // 隐藏项
            bool dir = it->is_directory(ec);
            tmp.push_back({ nm, dir });
        }
        std::stable_sort(tmp.begin(), tmp.end(), [](const Entry& a, const Entry& b) {
            if (a.isDir != b.isDir) return a.isDir;
            return a.name < b.name;
        });
        out.swap(tmp);
    };
    auto loadDir = [&](const std::wstring& d) {
        if (cache.count(d)) return;
        std::vector<Entry> e;
        listDir(d, e);
        cache[d] = std::move(e);
    };

    // 首次打开且未设根目录: 用当前文件目录或 IDE 源码目录
    if (root.empty()) {
        if (CodeEditor* ed = g_app.ActiveEditor())
            if (ed->HasPath()) root = utils::DirName(ed->m_path);
        if (root.empty()) root = L"e:\\cppIDE";
    }

    // 工具栏
    if (ImGui::Button(ImGui::GetContentRegionAvail().x < 180 ? "目录" : "打开文件夹")) {
        std::wstring p = PickFolderW();
        if (!p.empty()) {
            root = p;
            expanded.clear();
            cache.clear();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("刷新")) { cache.clear(); g_shellIcons.Clear(); }
    ImGui::TextDisabled("%s", utils::WToU8(utils::FileBase(root)).c_str());
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", utils::WToU8(root).c_str());
    ImGui::Separator();

    // 打开文件到编辑器
    auto openFile = [](const std::wstring& wp) {
        for (int i = 0; i < (int)g_app.editors.size(); i++)
            if (_wcsicmp(g_app.editors[i]->m_path.c_str(), wp.c_str()) == 0) {
                g_app.active = i;
                g_app.SyncDiagPanel();
                if (ImGuiWindow* w = ImGui::FindWindowByName("编辑器")) ImGui::FocusWindow(w);
                return;
            }
        g_app.NewFile(wp);
        g_app.MarkDiag();
        if (ImGuiWindow* w = ImGui::FindWindowByName("编辑器")) ImGui::FocusWindow(w);
    };

    // 递归绘制
    std::function<void(const std::wstring&, int)> drawDir;
    drawDir = [&](const std::wstring& dir, int depth) {
        loadDir(dir);
        for (auto& en : cache[dir]) {
            std::wstring full = dir + L"\\" + en.name;
            std::string nm  = utils::WToU8(en.name);
            float indent = (float)(depth * 14);
            if (en.isDir) {
                bool open = expanded.count(full) != 0;
                // 目录行
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + indent);
                ImVec2 row=ImGui::GetCursorScreenPos();
                GLuint icon=ImGui::IsRectVisible(ImVec2(20,22)) ? g_shellIcons.Get(full,true) : 0;
                ImGui::PushID(("D" + utils::WToU8(full)).c_str());
                if (ImGui::Selectable("##directory", false, 0, ImVec2(std::max(ImGui::GetContentRegionAvail().x,ImGui::CalcTextSize(nm.c_str()).x+42),22))) {
                    if (open) expanded.erase(full);
                    else { expanded.insert(full); cache.erase(full); }
                }
                auto* draw=ImGui::GetWindowDrawList(); ImU32 color=ImGui::GetColorU32(ImGuiCol_TextDisabled);
                if(open) draw->AddTriangleFilled(ImVec2(row.x+4,row.y+8),ImVec2(row.x+12,row.y+8),ImVec2(row.x+8,row.y+13),color);
                else draw->AddTriangleFilled(ImVec2(row.x+5,row.y+6),ImVec2(row.x+5,row.y+14),ImVec2(row.x+10,row.y+10),color);
                if(icon) draw->AddImage((ImTextureID)(intptr_t)icon,ImVec2(row.x+17,row.y+2),ImVec2(row.x+35,row.y+20));
                draw->AddText(ImVec2(row.x+40,row.y+3),ImGui::GetColorU32(ImGuiCol_Text),nm.c_str());
                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0) && !open) {
                    expanded.insert(full);
                    cache.erase(full);
                }
                if (ImGui::BeginPopupContextItem("dirctx")) {
                    std::wstring here = full;
                    if (ImGui::MenuItem("新建文件")) { ctxDir = here; modal = "newfile"; nameBuf.clear(); focusName = true; }
                    if (ImGui::MenuItem("新建文件夹")) { ctxDir = here; modal = "newdir"; nameBuf.clear(); focusName = true; }
                    ImGui::Separator();
                    if (ImGui::MenuItem("刷新")) { cache.erase(full); }
                    if (ImGui::MenuItem("在资源管理器中打开")) {
                        ShellExecuteW(nullptr, L"open", full.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                    }
                    ImGui::EndPopup();
                }
                ImGui::PopID();
                if (open) drawDir(full, depth + 1);
            } else {
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + indent + 10);
                ImGui::PushID(("F" + utils::WToU8(full)).c_str());
                ImVec2 row=ImGui::GetCursorScreenPos();
                GLuint icon=ImGui::IsRectVisible(ImVec2(20,22)) ? g_shellIcons.Get(full) : 0;
                bool selected=g_app.ActiveEditor() && _wcsicmp(g_app.ActiveEditor()->m_path.c_str(),full.c_str())==0;
                if (ImGui::Selectable("##file", selected, 0, ImVec2(std::max(ImGui::GetContentRegionAvail().x,ImGui::CalcTextSize(nm.c_str()).x+34),22))) openFile(full);
                if(icon) ImGui::GetWindowDrawList()->AddImage((ImTextureID)(intptr_t)icon,ImVec2(row.x+7,row.y+2),ImVec2(row.x+25,row.y+20));
                ImGui::GetWindowDrawList()->AddText(ImVec2(row.x+30,row.y+3),ImGui::GetColorU32(ImGuiCol_Text),nm.c_str());
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s",utils::WToU8(full).c_str());
                if (ImGui::BeginPopupContextItem("filectx")) {
                    std::wstring here = full;
                    if (ImGui::MenuItem("打开")) openFile(here);
                    if (ImGui::MenuItem("复制路径")) ImGui::SetClipboardText(utils::WToU8(here).c_str());
                    if (ImGui::MenuItem("在资源管理器中显示")) {
                        std::wstring args = L"/select,\"" + here + L"\"";
                        ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem("重命名")) { target = here; targetIsDir = false; modal = "rename"; nameBuf = nm; focusName = true; }
                    if (ImGui::MenuItem("删除")) { target = here; targetIsDir = false; modal = "del"; }
                    ImGui::EndPopup();
                }
                ImGui::PopID();
            }
        }
    };

    ImVec2 avail = ImGui::GetContentRegionAvail();
    if (ImGui::BeginChild("##ftree", avail, false, ImGuiWindowFlags_HorizontalScrollbar)) {
        drawDir(root, 0);
    }
    ImGui::EndChild();

    // ---- 新建/重命名 输入弹窗 ----
    if (!modal.empty() && modal != "del") {
        ImGui::OpenPopup(modal == "newfile" ? "新建文件" :
                         modal == "newdir"  ? "新建文件夹" : "重命名");
    }
    if (modal == "newfile" || modal == "newdir" || modal == "rename") {
        const char* title = modal == "newfile" ? "新建文件" :
                            modal == "newdir"  ? "新建文件夹" : "重命名";
        if (ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            if (focusName) { ImGui::SetKeyboardFocusHere(); focusName = false; }
            ui::InputTextStd("名称", nameBuf);
            bool ok = ImGui::Button("确定", ImVec2(90, 0));
            ImGui::SameLine();
            if (ImGui::Button("取消", ImVec2(90, 0))) modal.clear();
            if (ok && !nameBuf.empty()) {
                std::wstring nm = utils::U8ToW(nameBuf);
                // 简单防路径注入
                bool bad = nm.empty() || nm.find_first_of(L"\\/:*?\"<>|") != std::wstring::npos;
                if (!bad) {
                    if (modal == "rename") {
                        std::wstring np = utils::DirName(target) + L"\\" + nm;
                        std::error_code ec;
                        if (targetIsDir) fs::rename(target, np, ec);
                        else fs::rename(target, np, ec);
                        cache.erase(utils::DirName(target));   // 重建父目录
                    } else {
                        std::wstring dir = ctxDir.empty() ? root : ctxDir;
                        std::wstring np  = dir + L"\\" + nm;
                        if (modal == "newfile") {
                            if (utils::WriteFileU8(np, ""))
                                g_app.console.Append("[文件] 已创建: " + utils::WToU8(np) + "\n",
                                                     IM_COL32(140, 220, 150, 255));
                            cache.erase(dir);
                            if (GetFileAttributesW(dir.c_str()) != INVALID_FILE_ATTRIBUTES) openFile(np);
                        } else {
                            std::error_code ec;
                            fs::create_directories(np, ec);
                            cache.erase(dir);
                        }
                    }
                }
                modal.clear();
            }
            ImGui::EndPopup();
        }
    }
    // ---- 删除确认弹窗 ----
    if (modal == "del") ImGui::OpenPopup("确认删除");
    if (modal == "del" && ImGui::BeginPopupModal("确认删除", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("确定删除吗? (不可恢复)");
        ImGui::TextUnformatted(utils::WToU8(target).c_str());
        if (ImGui::Button("删除", ImVec2(90, 0))) {
            std::error_code ec;
            fs::remove_all(target, ec);
            cache.erase(utils::DirName(target));
            modal.clear();
        }
        ImGui::SameLine();
        if (ImGui::Button("取消", ImVec2(90, 0))) modal.clear();
        ImGui::EndPopup();
    }
    ImGui::End();
}

// ---------------- 恢复弹窗 ----------------
static void DrawRecoverModal() {
    if (g_app.recoverPending) {
        ImGui::OpenPopup("恢复");
        g_app.recoverPending = false;
    }
    if (ImGui::BeginPopupModal("恢复", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("选择需要恢复的标签。恢复为副本，不会覆盖原文件。");
        ImGui::BeginChild("##recovery_tabs",ImVec2(560,220),ImGuiChildFlags_Borders);
        for(size_t i=0;i<g_app.recoverSnapshot.tabs.size();++i) {
            const auto& tab=g_app.recoverSnapshot.tabs[i];bool chosen=g_app.recoverSelection[i]!=0;
            ImGui::PushID(int(i));if(ImGui::Checkbox(tab.name.c_str(),&chosen))g_app.recoverSelection[i]=chosen;
            if(!tab.path.empty())ImGui::TextDisabled("%s",tab.path.c_str());ImGui::PopID();
        }
        ImGui::EndChild();
        if(ImGui::Button("恢复选中标签")){g_app.ApplyRecovery(true);ImGui::CloseCurrentPopup();}
        ImGui::SameLine();if(ImGui::Button("忽略本次恢复")){g_app.ApplyRecovery(false);ImGui::CloseCurrentPopup();}
        ImGui::EndPopup();
    }
}

// ---------------- 关闭确认弹窗 ----------------
static void DrawCloseConfirmModal() {
    static int idx = -1;
    if (g_app.pendingClose >= 0) {
        idx = g_app.pendingClose;
        g_app.pendingClose = -1;   // 消费标记
        ImGui::OpenPopup("未保存的更改");
    }
    if (ImGui::BeginPopupModal("未保存的更改", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("该文件有未保存的修改, 确定关闭?");
        if (ImGui::Button("关闭", ImVec2(100, 0))) {
            g_app.RemoveEditorAt(idx);   // 由 App 统一修正 active 索引
            idx = -1;
            ImGui::CloseCurrentPopup();
            ImGuiWindow* w = ImGui::FindWindowByName("编辑器");
            if (w) ImGui::FocusWindow(w);
        }
        ImGui::SameLine();
        if (ImGui::Button("取消", ImVec2(100, 0))) {
            idx = -1;
            ImGui::CloseCurrentPopup();
            ImGuiWindow* w = ImGui::FindWindowByName("编辑器");
            if (w) ImGui::FocusWindow(w);
        }
        ImGui::EndPopup();
    }
}

// ---------------- 全局快捷键 ----------------
static void CycleEditor(int dir) {   // 下一个/上一个标签 (默认 Ctrl+]/Ctrl+[, 可在设置改)
    int n = (int)g_app.editors.size();
    if (n < 2 || g_app.active < 0) return;
    g_app.active = (g_app.active + dir + n) % n;
    g_app.SyncDiagPanel();
    if (ImGuiWindow* w = ImGui::FindWindowByName("编辑器")) ImGui::FocusWindow(w);
}

static void HandleGlobalShortcuts() {
    ImGuiIO& io = ImGui::GetIO();
    if (g_hkRecording) return;   // 设置窗口正在录制组合键, 避免把按键执行掉
    if(ImGui::GetTopMostPopupModal()) return;
    if (HotkeyDown("compile")) { g_app.showConsole=true; g_app.StartCompileRun(BuildAction::Compile); return; }
    if (HotkeyDown("run")) { g_app.showConsole=true; g_app.StartCompileRun(); return; }
    if (HotkeyDown("runOnly")) { g_app.showConsole=true; g_app.StartCompileRun(BuildAction::Run); return; }
    if (HotkeyDown("debug")) {
        g_app.showDebug=true;
        auto state=g_app.debugger.Snapshot();
        if(state.stopped) g_app.debugger.Continue(); else if(!state.active) g_app.StartCompileRun(BuildAction::Debug);
        return;
    }
    if (io.WantCaptureKeyboard) return;

    // 可配置全局动作 (修饰键要求完全匹配, 避免 Ctrl+X 同时触发 X)
    if (HotkeyDown("new"))     { g_app.NewFile();          return; }
    if (HotkeyDown("open"))    { g_app.OpenFileDialog();   return; }
    if (HotkeyDown("save"))    { g_app.SaveActive(false);  return; }
    if (HotkeyDown("saveAs"))  { g_app.SaveActive(true);   return; }
    if (HotkeyDown("backup"))  { g_app.BackupNow();        return; }
    if (HotkeyDown("nextTab")) { CycleEditor(1);           return; }
    if (HotkeyDown("prevTab")) { CycleEditor(-1);          return; }

    // 固定快捷键: Ctrl+M 插件市场; Ctrl+1..9 / Ctrl+0 = 打开最近列表第 1..10 项
    if (io.KeyCtrl && !io.KeyShift && !io.KeyAlt) {
        if (ImGui::IsKeyPressed(ImGuiKey_M)) { g_app.showMarket = !g_app.showMarket; return; }
        for (int d = 0; d <= 9; d++) {
            if (ImGui::IsKeyPressed((ImGuiKey)(ImGuiKey_0 + d))) {
                int idx = (d == 0) ? 9 : d - 1;
                if (idx >= 0 && idx < (int)g_app.recentFiles.size())
                    g_app.OpenRecentFile(g_app.recentFiles[idx]);
                break;
            }
        }
    }
}

// WinMain 入口 (配合 -mwindows)
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    return main();
}
