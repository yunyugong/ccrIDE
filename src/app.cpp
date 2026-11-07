#include "app.h"
#include "ui_helpers.h"
#include <thread>
#include <algorithm>
#include <imgui_internal.h>   // ImGui::FindWindowByName
#include <nlohmann/json.hpp>
#include <shellapi.h>
#include <filesystem>
#include "portable_paths.h"

App g_app;

// ===================================================================
//  控制台
// ===================================================================
void Console::Append(const std::string& t, ImU32 c) {
    std::string bounded = t.size() > MAX_BYTES ? t.substr(t.size() - MAX_BYTES) : t;
    textBytes += bounded.size();
    lines.push_back({c, std::move(bounded)});
    while (lines.size() > MAX_LINES || textBytes > MAX_BYTES) {
        textBytes -= lines.front().text.size(); lines.pop_front();
    }
}

void Console::Clear() { lines.clear(); textBytes = 0; }

// 从输出行解析 "文件名.cpp:行号" (g++ 风格, 兼容 Windows 盘符路径)
static bool ParseErrRef(const std::string& s, std::string& outPath, int& outLine) {
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
    size_t start = i;
    auto endsWithSrc = [&](const std::string& name) {
        const char* exts[] = { ".cpp", ".cc", ".cxx", ".c", ".h", ".hpp", ".hh" };
        for (auto e : exts) {
            size_t n = strlen(e);
            if (name.size() >= n && _stricmp(name.c_str() + name.size() - n, e) == 0) return true;
        }
        return false;
    };
    while (i < s.size()) {
        if (s[i] == ':' && i + 1 < s.size() && isdigit((unsigned char)s[i + 1])) {
            std::string name = s.substr(start, i - start);
            // 排除盘符 "X:" 之类: 文件名必须以源码扩展名结尾, 行号 > 0
            int ln = atoi(s.c_str() + i + 1);
            if (ln > 0 && !name.empty() && endsWithSrc(name)) {
                outPath = name;
                outLine = ln;
                return true;
            }
        }
        i++;
    }
    // MSVC 风格: "file.cpp(12): error C2065: ..." -> 双击也能跳转
    size_t lp = start;
    while ((lp = s.find('(', lp)) != std::string::npos) {
        size_t rp = s.find(')', lp + 1);
        if (rp == std::string::npos) break;
        std::string num = s.substr(lp + 1, rp - lp - 1);
        if (!num.empty() && num.find_first_not_of("0123456789") == std::string::npos) {
            std::string name = s.substr(start, lp - start);
            int ln = atoi(num.c_str());
            if (ln > 0 && !name.empty() && endsWithSrc(name)) {
                outPath = name;
                outLine = ln;
                return true;
            }
        }
        lp = rp + 1;
    }
    return false;
}

void Console::DrawWindow(bool* open, bool terminalView) {
    if (terminalView && g_app.focusTerminal) { ImGui::SetNextWindowFocus(); g_app.focusTerminal = false; }
    if (!ImGui::Begin(terminalView ? "终端" : "输出", open)) { ImGui::End(); return; }
    if (ImGui::Button("清空")) Clear();
    ImGui::SameLine();
    ImGui::Checkbox("自动滚动", &autoScroll);

    ImGui::Separator();

    // 运行中需要 stdin 输入时, 预留底部输入区 (高约 62px)
    bool interact = terminalView && g_app.runInteractive.load();
    ImVec2 bodySize = interact ? ImVec2(0, -64) : ImVec2(0, 0);

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
    ImGui::PushFont(g_app.editorFont ? g_app.editorFont : ImGui::GetFont());
    if (ImGui::BeginChild("##out_body", bodySize, false, ImGuiWindowFlags_HorizontalScrollbar)) {
        if (lines.empty()) ImGui::TextDisabled(terminalView ? "程序输出将显示在这里；运行时可在下方输入数据" : "构建日志将显示在这里 · F10 编译 / F11 编译并运行");
        for (auto& ln : lines) {
            ImGui::PushStyleColor(ImGuiCol_Text, (ImVec4)ImColor(ln.color));
            ImGui::TextUnformatted(ln.text.c_str());
            ImGui::PopStyleColor();
            // 双击含 file:line 的行 -> 打开/切换并跳转 (编译错误快速定位)
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
                std::string p;
                int l = 0;
                if (ParseErrRef(ln.text, p, l)) g_app.OpenAtFile(p, l);
            }
        }
        if (autoScroll) ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
    ImGui::PopFont();
    ImGui::PopStyleVar();

    // ---- 底部: 程序运行中的 stdin 输入区 ----
    if (interact) {
        ImGui::Separator();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.85f, 0.55f, 1));
        ImGui::TextUnformatted("程序运行中, 可输入数据 (回车发送到程序 stdin)");
        ImGui::PopStyleColor();
        bool send = false;
        ImGui::SetNextItemWidth(std::max(60.0f, ImGui::GetContentRegionAvail().x - 80));
        bool hitEnter = ui::InputTextStd("##stdin_input", stdinBuf,
                                          ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        if (ImGui::Button("发送", ImVec2(72, 0)) && !stdinBuf.empty()) send = true;
        // 回车总是发送 (含空行: 某些程序靠空行结束输入); 按钮仅在非空时发送
        if (hitEnter) send = true;
        if (send && g_app.runInteractive.load()) {
            g_app.QueueStdin(utils::U8ToW(stdinBuf));
            stdinBuf.clear();
        }
    }
    ImGui::End();
}

// ===================================================================
//  初始化
// ===================================================================
bool App::Init() {
    executableDir = utils::GetExeDir();
    portableMode = GetFileAttributesW((executableDir + L"\\portable.ini").c_str()) != INVALID_FILE_ATTRIBUTES;
    if (portableMode) {
        saveDir = executableDir + L"\\";
        dataDir = executableDir + L"\\data";
        pluginDir = executableDir + L"\\plugins";
    } else {
        if (!utils::SelectSaveDir(saveDir)) {
            wchar_t local[32768];
            DWORD count = GetEnvironmentVariableW(L"LOCALAPPDATA", local, 32768);
            if (!count || count >= 32768) { MessageBoxW(nullptr, L"无法确定可写的数据目录", L"ccrIDE", MB_ICONERROR); return false; }
            saveDir = std::wstring(local) + L"\\";
        }
        dataDir = saveDir + L"MiniCPP";
        pluginDir = L"E:\\cPP\\plugins"; // Keep the existing development installation compatible.
    }
    backupDir = dataDir + L"\\backup";
    std::error_code directoryError;
    std::filesystem::create_directories(backupDir, directoryError);
    std::wstring probe = dataDir + L"\\.write-probe-" + std::to_wstring(GetCurrentProcessId());
    HANDLE writable = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                  FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (directoryError || writable == INVALID_HANDLE_VALUE) {
        if (writable != INVALID_HANDLE_VALUE) CloseHandle(writable);
        MessageBoxW(nullptr, L"数据目录不可写。请将完整安装包解压到个人可写目录后启动。", L"ccrIDE", MB_ICONERROR);
        return false;
    }
    CloseHandle(writable);
    settingsPath = dataDir + L"\\settings.json";
    settings.Load(settingsPath);
    std::string artifactsJson;
    if(utils::ReadFileU8(dataDir+L"\\build-artifacts.json",artifactsJson)) try {
        auto records=nlohmann::json::parse(artifactsJson);
        if(records.is_object()) for(auto it=records.begin();it!=records.end();++it)
            if(it.value().is_string()) lastArtifacts[utils::U8ToW(it.key())]=utils::U8ToW(it.value().get<std::string>());
    } catch(...) {}

    // 2. g++ 查找
    std::wstring gpp;
    if (!settings.gppPath.empty()) gpp = portable::Resolve(executableDir, utils::U8ToW(settings.gppPath));
    if (gpp.empty() && portableMode) {
        auto bundled = executableDir + L"\\toolchain\\mingw64\\bin\\g++.exe";
        if (GetFileAttributesW(bundled.c_str()) != INVALID_FILE_ATTRIBUTES) gpp = bundled;
    }
    if (gpp.empty()) {
        std::vector<std::wstring> roots = {
            L"D:\\rred\\mingw64\\bin",
            L"E:\\rred\\mingw64\\bin",
            L"C:\\Program Files (x86)\\Dev-Cpp",
            L"C:\\Program Files\\Dev-Cpp",
            L"C:\\msys64\\ucrt64\\bin",
        };
        if (!utils::FindGpp(roots, gpp)) {
            wchar_t buf[MAX_PATH] = {0};
            if (SearchPathW(nullptr, L"g++.exe", nullptr, MAX_PATH, buf, nullptr) > 0)
                gpp = buf;
        }
    }
    if (!gpp.empty())
        settings.gppPath = utils::WToU8(gpp);
    else
        console.Append("[警告] 未找到 g++ 编译器, 请在 设置 中配置路径\n", IM_COL32(255, 200, 100, 255));

    // 3. 插件目录 (DLL 加载需在 ImGui 上下文创建后, 由 main 调用 ScanAndLoad)
    plugins.LoadBundledCatalog();

    // 4. 恢复检查
    std::string autosave;
    if (utils::ReadFileU8(dataDir + L"\\autosave.cpp", autosave) && !autosave.empty()) {
        recoverContent = autosave;
        recoverPending = true;
    }

    // 5. 恢复上次会话 (打开的标签列表), 无会话则新建空文件
    LoadSession();
    LoadRecent();   // 最近打开列表
    return true;
}

void App::LoadFonts() {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();   // 重建 (字号变更) 时先清空

    // 中文字体 (UI)
    std::string cjkFont;
    const wchar_t* candidates[] = {
        L"C:\\Windows\\Fonts\\msyh.ttc",
        L"C:\\Windows\\Fonts\\msyh.ttf",
        L"C:\\Windows\\Fonts\\simhei.ttf",
        L"C:\\Windows\\Fonts\\simsun.ttc",
    };
    for (auto c : candidates)
        if (GetFileAttributesW(c) != INVALID_FILE_ATTRIBUTES) { cjkFont = utils::WToU8(c); break; }

    uiFont = nullptr;
    if (!cjkFont.empty())
        uiFont = io.Fonts->AddFontFromFileTTF(cjkFont.c_str(), 15.0f, nullptr,
                                              io.Fonts->GetGlyphRangesChineseFull());
    if (!uiFont) uiFont = io.Fonts->AddFontDefault();

    // 编辑器字体: Consolas + 合并中文
    editorFont = nullptr;
    ImFontConfig cfg;
    cfg.OversampleH = 2;
    cfg.OversampleV = 1;
    const wchar_t* monoCandidates[] = {
        L"C:\\Windows\\Fonts\\consola.ttf",
        L"C:\\Windows\\Fonts\\cour.ttf",
    };
    for (auto c : monoCandidates) {
        std::string p = utils::WToU8(c);
        if (GetFileAttributesW(c) != INVALID_FILE_ATTRIBUTES) {
            editorFont = io.Fonts->AddFontFromFileTTF(p.c_str(), (float)settings.fontSize, &cfg,
                                                      io.Fonts->GetGlyphRangesDefault());
            if (editorFont) break;
        }
    }
    if (editorFont && !cjkFont.empty()) {
        ImFontConfig merge;
        merge.MergeMode = true;
        merge.OversampleH = 2;
        merge.OversampleV = 1;
        merge.GlyphOffset = ImVec2(0.0f, -1.0f);
        io.Fonts->AddFontFromFileTTF(cjkFont.c_str(), (float)settings.fontSize, &merge,
                                     io.Fonts->GetGlyphRangesChineseFull());
    }
    if (!editorFont) editorFont = uiFont;

    io.FontDefault = uiFont;
    io.Fonts->Build();
}

void App::ApplyTheme() {
    bool dark = (settings.theme != "light");
    if (dark) {
        ImGui::StyleColorsDark();
        colors.editorBg    = IM_COL32(30, 30, 32, 255);
        colors.curLineBg   = IM_COL32(43, 43, 48, 255);
        colors.gutterBg    = IM_COL32(26, 26, 28, 255);
        colors.lineNum     = IM_COL32(110, 110, 120, 255);
        colors.selection   = IM_COL32(38, 79, 120, 255);
        colors.findMatch   = IM_COL32(255, 190, 60, 90);
        colors.text        = IM_COL32(212, 212, 212, 255);
        colors.keyword     = IM_COL32(86, 156, 214, 255);
        colors.typeName    = IM_COL32(78, 201, 176, 255);
        colors.str         = IM_COL32(206, 145, 120, 255);
        colors.comment     = IM_COL32(106, 153, 85, 255);
        colors.number      = IM_COL32(181, 206, 168, 255);
        colors.preproc     = IM_COL32(197, 134, 192, 255);
        colors.caret       = IM_COL32(255, 255, 255, 255);
        colors.scrollTrack = IM_COL32(30, 30, 32, 255);
        colors.scrollThumb = IM_COL32(70, 70, 76, 255);
        colors.diagErrBg   = IM_COL32(140, 40, 40, 56);
        colors.diagWarnBg  = IM_COL32(130, 100, 25, 48);
        colors.diagErrMark = IM_COL32(235, 80, 80, 255);
        colors.diagWarnMark= IM_COL32(230, 190, 70, 255);
        colors.bracketBg   = IM_COL32(90, 150, 230, 60);
    } else {
        ImGui::StyleColorsLight();
        colors.editorBg    = IM_COL32(255, 255, 255, 255);
        colors.curLineBg   = IM_COL32(240, 240, 240, 255);
        colors.gutterBg    = IM_COL32(238, 238, 238, 255);
        colors.lineNum     = IM_COL32(136, 136, 136, 255);
        colors.selection   = IM_COL32(173, 214, 255, 255);
        colors.findMatch   = IM_COL32(255, 220, 0, 90);
        colors.text        = IM_COL32(0, 0, 0, 255);
        colors.keyword     = IM_COL32(0, 0, 255, 255);
        colors.typeName    = IM_COL32(0, 128, 128, 255);
        colors.str         = IM_COL32(163, 21, 21, 255);
        colors.comment     = IM_COL32(0, 128, 0, 255);
        colors.number      = IM_COL32(9, 134, 88, 255);
        colors.preproc     = IM_COL32(128, 0, 64, 255);
        colors.caret       = IM_COL32(0, 0, 0, 255);
        colors.scrollTrack = IM_COL32(235, 235, 235, 255);
        colors.scrollThumb = IM_COL32(168, 168, 168, 255);
        colors.diagErrBg   = IM_COL32(220, 60, 60, 44);
        colors.diagWarnBg  = IM_COL32(210, 160, 30, 40);
        colors.diagErrMark = IM_COL32(200, 45, 45, 255);
        colors.diagWarnMark= IM_COL32(190, 140, 20, 255);
        colors.bracketBg   = IM_COL32(60, 120, 210, 50);
    }
    auto& style = ImGui::GetStyle();
    style.WindowPadding = ImVec2(12, 10);
    style.FramePadding = ImVec2(10, 5);
    style.ItemSpacing = ImVec2(8, 6);
    style.ItemInnerSpacing = ImVec2(6, 4);
    style.WindowRounding = 5;
    style.ChildRounding = 3;
    style.FrameRounding = 5;
    style.PopupRounding = 6;
    style.TabRounding = 2;
    style.GrabRounding = 5;
    style.ScrollbarRounding = 7;
    style.ScrollbarSize = 11;
    style.WindowBorderSize = 1;
    style.FrameBorderSize = 0;
    style.TabBorderSize = 0;
    style.DockingNodeHasCloseButton = false; // Each panel tab already has its own close button.
    style.DockingSeparatorSize = 4;
    auto set = [&](ImGuiCol col, ImVec4 d, ImVec4 l) { style.Colors[col] = dark ? d : l; };
    set(ImGuiCol_WindowBg,       {0.145f,0.149f,0.165f,1}, {0.97f,0.98f,0.99f,1});
    set(ImGuiCol_ChildBg,        {0,0,0,0},               {0,0,0,0});
    set(ImGuiCol_PopupBg,        {0.17f,0.18f,0.20f,1},   {1,1,1,1});
    set(ImGuiCol_Text,           {0.86f,0.89f,0.93f,1},   {0.16f,0.20f,0.26f,1});
    set(ImGuiCol_TextDisabled,   {0.49f,0.56f,0.65f,1},   {0.43f,0.48f,0.55f,1});
    set(ImGuiCol_Border,         {0.22f,0.23f,0.26f,1},   {0.83f,0.86f,0.90f,1});
    set(ImGuiCol_MenuBarBg,      {0.10f,0.105f,0.12f,1},{0.94f,0.95f,0.97f,1});
    set(ImGuiCol_TitleBg,        {0.125f,0.13f,0.145f,1},   {0.93f,0.95f,0.97f,1});
    set(ImGuiCol_TitleBgActive,  {0.17f,0.18f,0.20f,1},   {0.88f,0.92f,0.96f,1});
    set(ImGuiCol_FrameBg,        {0.19f,0.20f,0.22f,1},   {0.91f,0.94f,0.97f,1});
    set(ImGuiCol_FrameBgHovered, {0.18f,0.24f,0.30f,1},   {0.85f,0.90f,0.95f,1});
    set(ImGuiCol_FrameBgActive,  {0.20f,0.28f,0.35f,1},   {0.79f,0.86f,0.93f,1});
    set(ImGuiCol_Button,         {0.20f,0.21f,0.235f,1},   {0.88f,0.92f,0.96f,1});
    set(ImGuiCol_ButtonHovered,  {0.20f,0.28f,0.35f,1},   {0.79f,0.86f,0.93f,1});
    set(ImGuiCol_ButtonActive,   {0.24f,0.34f,0.41f,1},   {0.71f,0.81f,0.91f,1});
    set(ImGuiCol_Header,         {0.19f,0.26f,0.36f,1},   {0.85f,0.91f,0.96f,1});
    set(ImGuiCol_HeaderHovered,  {0.20f,0.30f,0.37f,1},   {0.77f,0.86f,0.93f,1});
    set(ImGuiCol_HeaderActive,   {0.22f,0.36f,0.42f,1},   {0.69f,0.81f,0.90f,1});
    set(ImGuiCol_Tab,            {0.125f,0.13f,0.145f,1},   {0.91f,0.94f,0.97f,1});
    set(ImGuiCol_TabHovered,     {0.19f,0.27f,0.33f,1},   {0.80f,0.88f,0.94f,1});
    set(ImGuiCol_TabSelected,    {0.12f,0.125f,0.14f,1},   {1,1,1,1});
    set(ImGuiCol_TabDimmed,      {0.125f,0.13f,0.145f,1},   {0.91f,0.94f,0.97f,1});
    set(ImGuiCol_TabDimmedSelected,{0.14f,0.15f,0.17f,1}, {0.96f,0.98f,1,1});
    set(ImGuiCol_CheckMark,      {0.42f,0.64f,1.0f,1},   {0.10f,0.48f,0.40f,1});
    set(ImGuiCol_TabSelectedOverline,{0.42f,0.64f,1.0f,1},{0.10f,0.48f,0.40f,1});
    set(ImGuiCol_Separator,      {0.22f,0.23f,0.26f,1},   {0.83f,0.86f,0.90f,1});
    set(ImGuiCol_DockingPreview, {0.30f,0.54f,0.90f,0.5f},{0.15f,0.55f,0.45f,0.4f});
    if (dark) {
        colors.editorBg = IM_COL32(22,27,35,255);
        colors.gutterBg = IM_COL32(22,27,35,255);
        colors.curLineBg = IM_COL32(30,39,49,255);
        colors.lineNum = IM_COL32(95,111,130,255);
        colors.selection = IM_COL32(43,80,100,255);
        colors.scrollTrack = colors.editorBg;
        colors.scrollThumb = IM_COL32(60,75,92,255);
    }

    editorPalette = DefaultEditorPalette(dark);
    for (size_t i=0; i<editorPalette.size(); ++i) {
        auto key = settings.theme + "/" + EditorColorSpecs[i].key;
        auto value = settings.editorColors.find(key);
        if (value != settings.editorColors.end()) editorPalette[i] = value->second;
    }
    if (settings.editorColors.count(settings.theme + "/Text") && !settings.editorColors.count(settings.theme + "/Identifier"))
        editorPalette[(size_t)EditorColor::Identifier] = EditorColorValue(EditorColor::Text);
    colors.editorBg = EditorColorValue(EditorColor::Background);
    colors.curLineBg = EditorColorValue(EditorColor::CurrentLine);
    colors.gutterBg = EditorColorValue(EditorColor::Gutter);
    colors.lineNum = EditorColorValue(EditorColor::LineNumber);
    colors.selection = EditorColorValue(EditorColor::Selection);
    colors.findMatch = EditorColorValue(EditorColor::Find);
    colors.text = EditorColorValue(EditorColor::Text);
    colors.keyword = EditorColorValue(EditorColor::Keyword);
    colors.typeName = EditorColorValue(EditorColor::Class);
    colors.str = EditorColorValue(EditorColor::String);
    colors.comment = EditorColorValue(EditorColor::Comment);
    colors.number = EditorColorValue(EditorColor::Integer);
    colors.preproc = EditorColorValue(EditorColor::Preprocessor);
    colors.caret = EditorColorValue(EditorColor::Caret);
    colors.diagErrBg = EditorColorValue(EditorColor::ErrorBg);
    colors.diagWarnBg = EditorColorValue(EditorColor::WarningBg);
    colors.diagErrMark = EditorColorValue(EditorColor::Error);
    colors.diagWarnMark = EditorColorValue(EditorColor::Warning);
    colors.bracketBg = EditorColorValue(EditorColor::BracketMatch);

}

// ===================================================================
//  编辑器管理
// ===================================================================
CodeEditor* App::ActiveEditor() {
    if (active < 0 || active >= (int)editors.size()) return nullptr;
    return editors[active].get();
}

void App::NewFile(const std::wstring& path) {
    auto ed = std::make_unique<CodeEditor>();
    if (!path.empty()) {
        // 前置检查: 拒绝超大文件 (避免卡死/占满内存)
        unsigned long long sz = utils::FileSize(path);
        if (sz > 20ull * 1024 * 1024) {
            console.Append("[文件] 文件过大 (" + std::to_string(sz / 1024 / 1024) +
                           " MB), 已取消打开: " + utils::WToU8(path) + "\n",
                           IM_COL32(255, 120, 120, 255));
            return;
        }
        std::string content;
        if (!utils::ReadFileU8(path, content)) {
            console.Append("[文件] 打开失败 (不存在或无权限): " + utils::WToU8(path) + "\n",
                           IM_COL32(255, 120, 120, 255));
            return;
        }
        // 含 NUL 字节视为二进制文件, 拒绝打开 (避免空内容保存覆盖损坏原文件)
        if (content.find('\0') != std::string::npos) {
            console.Append("[文件] 似乎不是文本文件, 已取消打开: " + utils::WToU8(path) + "\n",
                           IM_COL32(255, 200, 100, 255));
            return;
        }
        // 编码识别: UTF-8 BOM -> UTF-8; 非 UTF-8 -> 按 GBK (CP936) 解码, 保存时保持
        ed->m_enc = 0;
        if (content.size() >= 3 &&
            (unsigned char)content[0] == 0xEF && (unsigned char)content[1] == 0xBB &&
            (unsigned char)content[2] == 0xBF) {
            content = content.substr(3);   // 去除 BOM
        } else if (!utils::IsValidUtf8(content)) {
            content = utils::GbkToUtf8(content);
            ed->m_enc = 1;
        }
        // 统一换行
        std::string t;
        t.reserve(content.size());
        for (size_t i = 0; i < content.size(); i++) {
            if (content[i] == '\r') continue;
            t += content[i];
        }
        ed->SetText(t);
        ed->SetPath(path);
    } else {
        ed->SetText("");
        ed->m_name = "未命名.cpp";
    }
    ed->RebuildLines();
    active = (int)editors.size();
    editors.push_back(std::move(ed));
}

void App::OpenFileDialog() {
    std::wstring path;
    if (!utils::OpenFileDialog(dataDir, path)) return;
    PushRecent(path);   // 最近打开列表 (去重置顶)
    // 已打开则切换 (Windows 路径大小写不敏感)
    for (int i = 0; i < (int)editors.size(); i++)
        if (_wcsicmp(editors[i]->m_path.c_str(), path.c_str()) == 0) { active = i; SyncDiagPanel(); return; }
    NewFile(path);
    console.Append("[文件] 已打开: " + utils::WToU8(path) + "\n", IM_COL32(140, 180, 255, 255));
    MarkDiag();   // 打开后触发一次自动语法检查
}

bool App::SaveActive(bool saveAs) {
    CodeEditor* ed = ActiveEditor();
    if (!ed) return false;
    bool wasUntitled = !ed->HasPath();   // 另存为 / 首次保存时记录新路径
    bool ok = saveAs ? ed->SaveAs() : ed->Save();
    if (ok) {
        console.Append("[文件] 已保存: " + utils::WToU8(ed->m_path) + "\n", IM_COL32(140, 220, 150, 255));
        ed->m_dirty = false;
        if (saveAs || wasUntitled) PushRecent(ed->m_path);   // 新文件名入最近列表
    }
    return ok;
}

// ---- 最近打开 (MRU) ----
void App::PushRecent(const std::wstring& path) {
    if (path.empty()) return;
    auto same = [&](const std::wstring& p) {
        return _wcsicmp(p.c_str(), path.c_str()) == 0;
    };
    recentFiles.erase(std::remove_if(recentFiles.begin(), recentFiles.end(), same), recentFiles.end());
    recentFiles.insert(recentFiles.begin(), path);
    if (recentFiles.size() > 10) recentFiles.resize(10);
    SaveRecent();
}

void App::SaveRecent() {
    using json = nlohmann::json;
    try {
        json arr = json::array();
        for (auto& p : recentFiles) arr.push_back(utils::WToU8(p));
        utils::WriteFileU8(dataDir + L"\\recent.json", arr.dump(2));
    } catch (...) {}
}

void App::LoadRecent() {
    using json = nlohmann::json;
    std::string s;
    if (!utils::ReadFileU8(dataDir + L"\\recent.json", s) || s.empty()) return;
    try {
        auto js = json::parse(s);
        if (!js.is_array()) return;
        recentFiles.clear();
        for (auto& f : js) {
            if (!f.is_string()) continue;
            std::wstring wp = utils::U8ToW(f.get<std::string>());
            if (wp.empty()) continue;
            // 只保留仍存在的文件, 避免死链接
            if (GetFileAttributesW(wp.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
            // 去重 (配置可能被手工编辑)
            bool dup = false;
            for (auto& r : recentFiles) if (_wcsicmp(r.c_str(), wp.c_str()) == 0) { dup = true; break; }
            if (dup) continue;
            recentFiles.push_back(wp);
            if (recentFiles.size() >= 10) break;
        }
    } catch (...) {}
}

void App::OpenRecentFile(const std::wstring& path) {
    if (path.empty()) return;
    PushRecent(path);   // 点击即置顶
    for (int i = 0; i < (int)editors.size(); i++) {
        if (_wcsicmp(editors[i]->m_path.c_str(), path.c_str()) == 0) {
            active = i;
            SyncDiagPanel();
            if (ImGuiWindow* w = ImGui::FindWindowByName("编辑器")) ImGui::FocusWindow(w);
            return;
        }
    }
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        console.Append("[文件] 最近文件不存在: " + utils::WToU8(path) + "\n", IM_COL32(255, 120, 120, 255));
        return;
    }
    NewFile(path);
    MarkDiag();
}

void App::QueueStdin(const std::wstring& line) {
    std::lock_guard<std::mutex> lk(stdinMutex);
    size_t pending = line.size();
    for (const auto& item : stdinQueue) pending += item.size();
    if (pending > 16384) {
        console.Append("[输入] 待发送内容过多，请等待程序读取后重试\n", IM_COL32(255,190,60,255));
        return;
    }
    stdinQueue.push_back(line);
}

void App::CloseEditor(int idx) {
    if (idx < 0 || idx >= (int)editors.size()) return;
    if (editors[idx]->m_dirty) {
        pendingClose = idx;   // 弹窗确认
        return;
    }
    RemoveEditorAt(idx);
}

// 移除标签并修正 active: 删除 idx 后, 其后的标签整体前移,
// 因此 active > idx 必须减 1 (原实现会跳到后一个标签)
void App::RemoveEditorAt(int idx) {
    if (idx < 0 || idx >= (int)editors.size()) return;
    editors.erase(editors.begin() + idx);
    if (editors.empty()) { active = -1; NewFile(); return; }
    if (active > idx) --active;
    if (active >= (int)editors.size()) active = (int)editors.size() - 1;
    if (active < 0) active = 0;
    SyncDiagPanel();
}

void App::BackupNow() {
    CodeEditor* ed = ActiveEditor();
    if (!ed) return;
    std::wstring name = utils::BackupName();
    if (utils::WriteFileU8(backupDir + L"\\" + name, ed->m_text)) {
        console.Append("[备份] " + utils::WToU8(name) + "\n", IM_COL32(200, 170, 255, 255));
    } else {
        console.Append("[备份] 失败\n", IM_COL32(255, 120, 120, 255));
    }
}

// ===================================================================
//  自动语法检查 (输入停顿 0.8s 后台 g++ -fsyntax-only, 同小熊猫C++)
// ===================================================================

// 解析 g++ 诊断行: "<file>:<line>:<col>: error|warning: <msg>"
static bool ParseDiagLine(const std::string& line, const std::string& fileKey, Diag& d) {
    struct Sev { const char* s; bool err; };
    static const Sev sevs[] = {
        { ": fatal error: ", true }, { ": error: ", true }, { ": warning: ", false } };
    size_t best = std::string::npos; int bi = -1;
    for (int i = 0; i < 3; i++) {
        size_t p = line.find(sevs[i].s);
        if (p != std::string::npos && (best == std::string::npos || p < best)) { best = p; bi = i; }
    }
    if (bi < 0) return false;
    size_t p2 = line.rfind(':', best - 1);
    if (p2 == std::string::npos || p2 == 0) return false;
    std::string colS = line.substr(p2 + 1, best - p2 - 1);
    if (colS.empty() || colS.find_first_not_of("0123456789") != std::string::npos) return false;
    size_t p1 = line.rfind(':', p2 - 1);
    if (p1 == std::string::npos) return false;
    std::string lineS = line.substr(p1 + 1, p2 - p1 - 1);
    if (lineS.empty() || lineS.find_first_not_of("0123456789") != std::string::npos) return false;
    std::string file = line.substr(0, p1);
    // 文件尾匹配 (大小写/分隔符差异兜底)
    if (file.size() < fileKey.size() ||
        file.compare(file.size() - fileKey.size(), fileKey.size(), fileKey) != 0) return false;
    d.isErr = sevs[bi].err;
    d.line = atoi(lineS.c_str());
    d.col  = atoi(colS.c_str());
    d.msg  = line.substr(best + strlen(sevs[bi].s));
    // 去首尾空白
    size_t b = d.msg.find_first_not_of(" \t\r");
    size_t e = d.msg.find_last_not_of(" \t\r");
    d.msg = (b == std::string::npos) ? "" : d.msg.substr(b, e - b + 1);
    return d.line > 0;
}

static std::vector<Diag> ParseDiags(const std::string& text, const std::string& fileKey) {
    std::vector<Diag> out;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        std::string line = text.substr(pos, (nl == std::string::npos ? text.size() : nl) - pos);
        Diag d;
        if (ParseDiagLine(line, fileKey, d)) out.push_back(std::move(d));
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    std::stable_sort(out.begin(), out.end(),
                     [](const Diag& a, const Diag& b) { return a.line < b.line; });
    return out;
}

void App::MarkDiag() {
    editRev++;
    diagPending = true;
    diagTimer = 0;
}

void App::DiagTick(float dt) {
    if (!settings.autoCheck) { diagPending = false; return; }
    if (!diagPending || diagBusy.load()) return;
    diagTimer += dt;
    if (diagTimer < 0.8f) return;
    diagPending = false;
    diagTimer = 0;
    StartDiagCheck();
}

void App::StartDiagCheck() {
    if (!settings.autoCheck) return;
    CodeEditor* ed = ActiveEditor();
    if (!ed) return;
    std::wstring gpp = utils::U8ToW(settings.gppPath);
    if (gpp.empty()) return;
    bool expected = false;
    if (!diagBusy.compare_exchange_strong(expected, true)) { diagPending = true; return; }

    // 临时文件写到源文件同目录, 保证 quoted include 可用; 未命名文件写 dataDir
    // 文件名带标签 uid: 多实例/多标签同目录时互不覆盖; 检查结束后删除, 不残留
    std::wstring dir = ed->HasPath() ? utils::DirName(ed->m_path) : dataDir;
    std::wstring tmp = dir + L"\\.minicpp_syn_" + std::to_wstring(ed->m_uid) + L".cpp";
    if (!utils::WriteFileU8(tmp, ed->m_text)) { diagBusy.store(false); diagStatus.clear(); return; }

    uint64_t rev = editRev;
    diagStatus = "检查中...";
    if (diagThread.joinable()) diagThread.join();
    diagThread = std::thread([this, gpp, tmp, dir, rev]() {
        std::wstring outF = dataDir + L"\\syn_out.txt", errF = dataDir + L"\\syn_err.txt";
        auto r = utils::RunProcess(gpp, { tmp, L"-fsyntax-only", L"-std=c++17" },
                                   dir, outF, errF, 30000, true, &shuttingDown);
        std::vector<Diag> diags = ParseDiags(r.out + "\n" + r.err, utils::WToU8(tmp));
        DeleteFileW(tmp.c_str());   // 检查结束即清理临时文件
        { std::lock_guard<std::mutex> lk(diagMutex); diagResults = std::move(diags); diagDoneRev = rev; diagAutomatic = true; }
        diagBusy.store(false);
    });
}

void App::PumpDiagResults() {
    std::vector<Diag> d;
    uint64_t rev = 0;
    bool automatic = true;
    { std::lock_guard<std::mutex> lk(diagMutex); d.swap(diagResults); rev = diagDoneRev; automatic = diagAutomatic; diagDoneRev = 0; }
    if (rev == 0) return;
    if (automatic && !settings.autoCheck) return;
    if (rev != editRev) {
        // 检查期间又编辑了 -> 丢弃本次结果, 但必须重新排一次检查, 否则结果会永久过期
        diagPending = true;
        diagTimer = 0;
        return;
    }
    CodeEditor* ed = ActiveEditor();
    if (ed) ed->m_diags = d;
    activeDiags = d;
    int ne = 0, nw = 0;
    for (auto& x : d) x.isErr ? ne++ : nw++;
    if (ne)      diagStatus = std::to_string(ne) + " 错误" + (nw ? ", " + std::to_string(nw) + " 警告" : "");
    else if (nw) diagStatus = std::to_string(nw) + " 警告";
    else         diagStatus = "无错误";
}

void App::SyncDiagPanel() {
    CodeEditor* ed = ActiveEditor();
    activeDiags = ed ? ed->m_diags : std::vector<Diag>{};
}

void App::GotoEditorLine(int line) {
    CodeEditor* ed = ActiveEditor();
    if (!ed) return;
    ed->GotoLine(line);
    if (ImGuiWindow* w = ImGui::FindWindowByName("编辑器")) ImGui::FocusWindow(w);
}

// 打开/切换到指定文件并跳转行 (输出面板双击 file:line)
void App::OpenAtFile(const std::string& pathU8, int line) {
    if (pathU8.empty() || line <= 0) return;
    std::wstring wp = utils::U8ToW(pathU8);
    // 相对路径 -> 相对数据目录解析
    if (wp.find(L':') == std::wstring::npos &&
        wp[0] != L'\\' && wp[0] != L'/')
        wp = dataDir + L"\\" + wp;
    if (GetFileAttributesW(wp.c_str()) == INVALID_FILE_ATTRIBUTES) return;

    int found = -1;
    for (int k = 0; k < (int)editors.size(); k++)
        if (_wcsicmp(editors[k]->m_path.c_str(), wp.c_str()) == 0) { found = k; break; }
    if (found < 0) {
        NewFile(wp);
        found = (int)editors.size() - 1;
    }
    if (found >= 0 && found < (int)editors.size()) {
        active = found;
        SyncDiagPanel();
        editors[found]->GotoLine(line);
        if (ImGuiWindow* w = ImGui::FindWindowByName("编辑器")) ImGui::FocusWindow(w);
    }
}

// ===================================================================
//  F5 编译运行 (后台线程)
// ===================================================================

// Compilation, interactive runs and background tools share execution::Run.
static void RunInteractiveProc(execution::Request request) {
    request.capture = false;
    request.interactive = true;
    request.cancel = &g_app.killRequested;
    request.onOutput = [](const std::string& text, bool isErr) {
        g_app.QueueProcOutput(text, isErr ? IM_COL32(255,220,120,255) : IM_COL32(230,230,230,255), true);
    };
    request.pollInput = []() {
        std::vector<std::wstring> lines;
        { std::lock_guard<std::mutex> lock(g_app.stdinMutex); lines.swap(g_app.stdinQueue); }
        std::string input;
        for (const auto& line : lines) input += utils::WToU8(line) + "\n";
        if (!input.empty()) g_app.QueueProcOutput("[输入] " + input, IM_COL32(120,220,180,255), true);
        return input;
    };
    g_app.runInteractive.store(true);
    g_app.SetProcStatus("运行中 (可输入数据或停止)");
    auto result = execution::Run(request);
    g_app.runInteractive.store(false);
    std::string summary = "\n[运行] " + std::string(execution::ReasonText(result.reason)) +
        "，耗时 " + std::to_string(result.elapsedMs) + " ms，峰值提交内存 " +
        std::to_string(result.peakMemoryBytes / 1024) + " KiB";
    if (result.started) summary += "，退出码 " + std::to_string((int)result.exitCode);
    if (result.exitCode == 0xC0000005UL) summary += " (访问冲突)";
    if (result.exitCode == 0xC00000FDUL) summary += " (栈溢出)";
    if (!result.error.empty()) summary += "\n" + result.error;
    g_app.QueueProcOutput(summary + "\n", result.reason == execution::Reason::Exited && result.exitCode == 0 ?
                         IM_COL32(140,220,150,255) : IM_COL32(255,160,100,255), true);
}

// ===================================================================
//  编译命令构造 (F5): 标准选项 或 自定义命令行模板
// ===================================================================
static const wchar_t* kStdList[] = { L"-std=c++17", L"-std=c++20", L"-std=c++14", L"-std=c++11", L"-std=c++23" };
static const wchar_t* kOptList[] = { L"", L"-O2", L"-O0", L"-O1", L"-O3", L"-Os" };
static const wchar_t kDefaultTemplate[] = L"{gpp} {std} {opt} {warn} {dbg} {extra} {file} -o {exe}";

// 引号感知分词: 去掉引号, 引号内的空格不拆 (也用于附加参数拆分)
static std::vector<std::wstring> SplitArgsQuoted(const std::wstring& s) {
    std::vector<std::wstring> out;
    std::wstring cur;
    bool inq = false;
    for (wchar_t c : s) {
        if (c == L'"') { inq = !inq; }
        else if ((c == L' ' || c == L'\t') && !inq) {
            if (!cur.empty()) { out.push_back(cur); cur.clear(); }
        } else cur += c;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

// 展开命令行模板 -> out[0] 为可执行文件, 其余为参数
// 支持的占位符: {gpp} {file} {exe} {dir} {std} {opt} {warn} {dbg} {extra}
static void ExpandTemplate(const std::wstring& tpl,
                           const std::wstring& gpp, const std::wstring& file,
                           const std::wstring& exePath, const std::wstring& dir,
                           std::vector<std::wstring>& out) {
    Settings& st = g_app.settings;
    auto value = [&](const std::wstring& key) -> std::wstring {
        if (key == L"{gpp}")   return gpp;
        if (key == L"{file}")  return file;
        if (key == L"{exe}")   return exePath;
        if (key == L"{dir}")   return dir;
        if (key == L"{std}")   return (st.stdVersion >= 0 && st.stdVersion < 5) ? kStdList[st.stdVersion] : L"";
        if (key == L"{opt}")   return (st.optLevel >= 0 && st.optLevel <= 5) ? kOptList[st.optLevel] : L"";
        if (key == L"{dbg}")   return st.dbgFlag ? L"-g" : L"";
        if (key == L"{warn}")  return st.warnAll ? L"-Wall -Wextra" : L"";
        if (key == L"{extra}") return utils::U8ToW(st.extraFlags);
        return L"";
    };
    auto expandable = [](const std::wstring& key) {   // 可能展开成多个参数的占位符
        return key == L"{warn}" || key == L"{extra}";
    };

    for (auto& t : SplitArgsQuoted(tpl)) {
        // 整体占位符 (单独一个词)
        if (t.size() > 2 && t.front() == L'{' && t.back() == L'}') {
            std::wstring v = value(t);
            if (v.empty()) continue;
            if (expandable(t)) {
                for (auto& e : SplitArgsQuoted(v)) out.push_back(std::move(e));
            } else {
                out.push_back(std::move(v));
            }
            continue;
        }
        // 与其它文本混合的 {xxx} -> 原位替换 (通常用于路径)
        std::wstring r = t;
        for (const wchar_t* nm : { L"gpp", L"file", L"exe", L"dir", L"std", L"opt", L"dbg", L"warn", L"extra" }) {
            std::wstring pat = L"{"; pat += nm; pat += L"}";
            std::wstring val = value(pat);
            size_t pos = 0;
            while ((pos = r.find(pat, pos)) != std::wstring::npos) {
                r.replace(pos, pat.size(), val);
                pos += val.size();
            }
        }
        if (!r.empty()) out.push_back(std::move(r));
    }
}

std::vector<std::pair<std::string,int>> App::DebugBreakpoints() const {
    std::vector<std::pair<std::string,int>> result;
    for(const auto& editor:editors) if(editor->HasPath()) for(int line:editor->m_breakpoints)
        result.emplace_back(utils::WToU8(editor->m_path),line);
    return result;
}

void App::StartCompileRun(BuildAction action) {
    if (procBusy.load() || debugger.Snapshot().active) {
        console.Append("[编译] 已有任务进行中\n", IM_COL32(255, 200, 100, 255));
        return;
    }
    CodeEditor* ed = ActiveEditor();
    if (!ed) return;
    if (action != BuildAction::Run && !SaveActive(false)) return;
    terminalRevealed = false;
    if(action==BuildAction::Run && !ed->HasPath()) { console.Append("[运行] 请先编译当前文件\n",IM_COL32(255,190,60,255)); return; }

    std::wstring file = ed->m_path;
    std::wstring dir = utils::DirName(file);
    std::wstring exePath;
    if(action==BuildAction::Run) {
        std::lock_guard<std::mutex> lock(artifactMutex);
        auto found=lastArtifacts.find(file);
        if(found!=lastArtifacts.end()) exePath=found->second;
        if(exePath.empty() || GetFileAttributesW(exePath.c_str())==INVALID_FILE_ATTRIBUTES) {
            console.Append("[运行] 当前文件没有已成功编译的程序，请先按 F10 编译\n",IM_COL32(255,190,60,255)); return;
        }
    } else {
        auto builds=std::filesystem::path(dir)/L".ccrIDE-build";
        std::error_code error; std::filesystem::create_directories(builds,error);
        if(error) { console.Append("[编译] 无法创建构建目录\n",IM_COL32(255,120,120,255)); return; }
        SYSTEMTIME now; GetLocalTime(&now);
        wchar_t stamp[128]; swprintf(stamp,128,L"-v%04u%02u%02u.%02u%02u%02u.%03u-%lu-%llu.exe",now.wYear,now.wMonth,now.wDay,now.wHour,now.wMinute,now.wSecond,now.wMilliseconds,GetCurrentProcessId(),(unsigned long long)GetTickCount64());
        exePath=(builds/(std::filesystem::path(file).stem().wstring()+stamp)).wstring();
    }
    std::wstring gpp = utils::U8ToW(settings.gppPath);
    bool useTpl = settings.useCmdTemplate && action!=BuildAction::Debug;
    if (action!=BuildAction::Run && !useTpl && (gpp.empty() || GetFileAttributesW(gpp.c_str()) == INVALID_FILE_ATTRIBUTES)) {
        console.Append("[编译] 找不到 g++ 编译器 (设置 -> g++ 路径)\n", IM_COL32(255, 120, 120, 255));
        return;
    }

    execution::Request runRequest;
    runRequest.exe = exePath;
    runRequest.cwd = settings.runDirectory.empty() ? dir : utils::U8ToW(settings.runDirectory);
    runRequest.limits.timeMs = (DWORD)std::clamp(settings.runTimeLimitMs, 100, 3600000);
    runRequest.limits.memoryBytes = (size_t)std::clamp(settings.runMemoryLimitMB, 0, 16384) * 1024 * 1024;
    runRequest.limits.outputBytes = (size_t)std::clamp(settings.runOutputLimitMB, 1, 64) * 1024 * 1024;
    if (!settings.runArguments.empty()) {
        int count = 0;
        std::wstring command = L"program " + utils::U8ToW(settings.runArguments);
        auto argv = CommandLineToArgvW(command.c_str(), &count);
        if (!argv) { console.Append("[运行配置] 无法解析程序参数\n", IM_COL32(255,120,120,255)); return; }
        for (int i = 1; i < count; ++i) runRequest.args.push_back(argv[i]);
        LocalFree(argv);
    }
    size_t pos = 0;
    while (pos < settings.runEnvironment.size()) {
        size_t end = settings.runEnvironment.find('\n', pos);
        std::string line = settings.runEnvironment.substr(pos, end == std::string::npos ? end : end - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) {
            size_t equal = line.find('=');
            if (equal == std::string::npos || equal == 0) {
                console.Append("[运行配置] 环境覆盖应每行填写 NAME=value\n", IM_COL32(255,120,120,255)); return;
            }
            runRequest.environment[utils::U8ToW(line.substr(0, equal))] = utils::U8ToW(line.substr(equal + 1));
        }
        if (end == std::string::npos) break;
        pos = end + 1;
    }
    // Generated programs may depend on the compiler's libstdc++/winpthread DLLs.
    // Pass a task-local PATH rather than changing the IDE or system environment.
    std::wstring inheritedPath;
    wchar_t pathBuffer[32768];
    DWORD pathLength = GetEnvironmentVariableW(L"PATH", pathBuffer, 32768);
    if (pathLength && pathLength < 32768) inheritedPath.assign(pathBuffer, pathLength);
    for (auto it = runRequest.environment.begin(); it != runRequest.environment.end();) {
        if (_wcsicmp(it->first.c_str(), L"PATH") == 0) { inheritedPath = it->second; it = runRequest.environment.erase(it); }
        else ++it;
    }
    runRequest.environment[L"PATH"] = utils::DirName(gpp) + L";" + inheritedPath;

    std::wstring gdb=settings.gdbPath.empty() ? utils::DirName(gpp)+L"\\gdb.exe" : portable::Resolve(executableDir,utils::U8ToW(settings.gdbPath));
    std::wstring debugInput;
    if(action==BuildAction::Debug) {
        if(GetFileAttributesW(gdb.c_str())==INVALID_FILE_ATTRIBUTES) { console.Append("[调试] 找不到 GDB，请在设置中指定路径\n",IM_COL32(255,120,120,255)); return; }
        debugInput=settings.debugInputFile.empty() ? dataDir+L"\\debug-empty-input.txt" : utils::U8ToW(settings.debugInputFile);
        if(settings.debugInputFile.empty()) utils::WriteFileU8(debugInput,"");
        if(GetFileAttributesW(debugInput.c_str())==INVALID_FILE_ATTRIBUTES) { console.Append("[调试] 输入文件不存在\n",IM_COL32(255,120,120,255)); return; }
    }

    if(action==BuildAction::Run) {
        if(procThread.joinable()) procThread.join();
        procBusy.store(true); killRequested.store(false);
        { std::lock_guard<std::mutex> lock(stdinMutex); stdinQueue.clear(); }
        if(ed->m_dirty) console.Append("[运行] 使用上次编译的版本，当前编辑内容尚未编译\n",IM_COL32(255,190,60,255));
        console.Append("[运行已编译程序] "+utils::WToU8(execution::CommandLine(exePath,runRequest.args))+"\n[运行目录] "+utils::WToU8(runRequest.cwd)+"\n",IM_COL32(140,200,255,255));
        procThread=std::thread([this,runRequest]() { RunInteractiveProc(runRequest); procBusy.store(false); });
        return;
    }

    console.Append("──────────────────────────────────────────\n", IM_COL32(120, 120, 130, 255));
    console.Append("[编译] " + utils::WToU8(file) + "\n", IM_COL32(140, 180, 255, 255));
    procBusy.store(true);
    SetProcStatus("编译中...");
    runInteractive.store(false);
    killRequested.store(false);   // 清除上次「终止」请求
    { std::lock_guard<std::mutex> lk(stdinMutex); stdinQueue.clear(); }   // 丢弃上次残留输入

    // procBusy 为 false 说明上一个线程已结束, 先回收再启动新线程
    if (procThread.joinable()) procThread.join();

    // 按设置构造编译命令: 标准选项 或 自定义命令行模板
    std::wstring exeCmd;                   // 编译器可执行文件
    std::vector<std::wstring> runArgs;     // 编译参数 (不含编译器本身)
    if (useTpl) {
        std::wstring tpl = utils::U8ToW(settings.commandTemplate);
        if (tpl.empty()) tpl = kDefaultTemplate;   // 留空 = 用内置默认模板
        std::vector<std::wstring> toks;
        ExpandTemplate(tpl, gpp, file, exePath, dir, toks);
        if (toks.empty()) {
            console.Append("[编译] 命令行模板展开结果为空\n", IM_COL32(255, 120, 120, 255));
            procBusy.store(false);
            SetProcStatus("");
            return;
        }
        exeCmd = toks[0];
        runArgs.assign(toks.begin() + 1, toks.end());
    } else {
        exeCmd = gpp;
        runArgs.push_back(file);
        if (settings.stdVersion >= 0 && settings.stdVersion < 5)
            runArgs.push_back(kStdList[settings.stdVersion]);
        if (settings.optLevel > 0 && settings.optLevel <= 5)
            runArgs.push_back(kOptList[settings.optLevel]);
        if (settings.warnAll) { runArgs.push_back(L"-Wall"); runArgs.push_back(L"-Wextra"); }
        if (settings.dbgFlag) runArgs.push_back(L"-g");
        for (auto& e : SplitArgsQuoted(utils::U8ToW(settings.extraFlags)))
            runArgs.push_back(std::move(e));
        if(action==BuildAction::Debug) { runArgs.push_back(L"-g"); runArgs.push_back(L"-O0"); runArgs.push_back(L"-fno-omit-frame-pointer"); }
        runArgs.push_back(L"-o");
        runArgs.push_back(exePath);
    }

    const DWORD compileTimeout = (DWORD)std::clamp(settings.compileTimeLimitMs, 100, 3600000);
    const uint64_t compileRev = editRev;
    console.Append("[命令] " + utils::WToU8(execution::CommandLine(exeCmd, runArgs)) + "\n[编译目录] " + utils::WToU8(dir) + "\n", IM_COL32(140,180,255,255));
    const auto breakpoints=DebugBreakpoints();
    procThread = std::thread([this, exeCmd, runArgs, file, exePath, dir, runRequest, compileTimeout, compileRev, action, gdb, debugInput, breakpoints]() {
        std::vector<ProcOut> outs;
        auto add = [&](const std::string& t, ImU32 c) { outs.push_back({ t, c }); };
        auto flush = [&]() {   // 一次性批量上屏 (编译阶段用, 避免逐条加锁)
            if (outs.empty()) return;
            for (const auto& output : outs) QueueProcOutput(output.text, output.color);
            outs.clear();
        };
        std::wstring outF = dataDir + L"\\build_out.txt";
        std::wstring errF = dataDir + L"\\build_err.txt";

        auto r = utils::RunProcess(exeCmd, runArgs, dir, outF, errF, compileTimeout, true, &killRequested);
        add("[编译] " + std::string(execution::ReasonText(r.reason)) + "，耗时 " + std::to_string(r.elapsedMs) + " ms\n", IM_COL32(160,180,200,255));
        if (!r.out.empty()) add(r.out, IM_COL32(230,230,230,255));
        if (!r.ok) {
            add("[编译] 无法启动编译器: " + r.error + "\n", IM_COL32(255, 120, 120, 255));
            flush();
        } else if (r.exitCode != 0) {
            add("[编译] 失败 (退出码 " + std::to_string(r.exitCode) + ")\n", IM_COL32(255, 120, 120, 255));
            add(r.err, IM_COL32(255, 120, 120, 255));
            if (!r.err.empty() && r.err.back() != '\n') add("\n", IM_COL32(255, 120, 120, 255));
            // 编译错误同步到诊断 (问题面板可点击跳转)
            auto diags = ParseDiags(r.err, utils::WToU8(file));
            { std::lock_guard<std::mutex> lk(diagMutex); diagResults = std::move(diags); diagDoneRev = compileRev; diagAutomatic = false; }
            flush();
        } else {
            if(GetFileAttributesW(exePath.c_str())==INVALID_FILE_ATTRIBUTES) {
                add("[编译] 编译器返回成功，但没有生成目标 EXE\n",IM_COL32(255,120,120,255)); flush(); procBusy.store(false); return;
            }
            auto successfulDiags=ParseDiags(r.err,utils::WToU8(file));
            { std::lock_guard<std::mutex> lock(diagMutex); diagResults=std::move(successfulDiags); diagDoneRev=compileRev; diagAutomatic=false; }
            { std::lock_guard<std::mutex> lock(artifactMutex);
                lastArtifacts[file]=exePath;
                nlohmann::json artifacts=nlohmann::json::object();
                for(const auto& artifact:lastArtifacts) artifacts[utils::WToU8(artifact.first)]=utils::WToU8(artifact.second);
                if(!utils::WriteFileU8(dataDir+L"\\build-artifacts.json",artifacts.dump(2))) add("[构建记录] 无法保存产物索引\n",IM_COL32(255,190,60,255));
            }
            if (!r.err.empty()) add(r.err, IM_COL32(255,220,120,255));
            add("[编译] 成功\n\n", IM_COL32(140, 220, 150, 255));
            add("[产物] " + utils::WToU8(exePath) + "\n\n", IM_COL32(140, 200, 255, 255));
            if(action==BuildAction::Compile) { flush(); procBusy.store(false); return; }
            if(action==BuildAction::Debug) {
                flush();
                if(!killRequested.load()) debugger.Start(gdb,runRequest,breakpoints,debugInput);
                procBusy.store(false); return;
            }
            if (GetFileAttributesW(exePath.c_str()) == INVALID_FILE_ATTRIBUTES) {
                add("[运行] exe 不存在\n", IM_COL32(255, 120, 120, 255));
                flush();
            } else {
                flush();   // 编译信息先上屏, 随后进入交互运行
                QueueProcOutput("[命令] " + utils::WToU8(execution::CommandLine(runRequest.exe, runRequest.args)) +
                    "\n[运行目录] " + utils::WToU8(runRequest.cwd) + "\n[环境] 继承宿主，覆盖 " +
                    std::to_string(runRequest.environment.size()) + " 项 (值在设置中查看，不写入日志)\n[限制] " +
                    std::to_string(runRequest.limits.timeMs) + " ms，内存 " +
                    std::to_string(runRequest.limits.memoryBytes / 1024 / 1024) + " MiB (0=不限制)，输出 " +
                    std::to_string(runRequest.limits.outputBytes / 1024 / 1024) + " MiB\n", IM_COL32(140,180,255,255), true);
                RunInteractiveProc(runRequest);
            }
        }
        procBusy.store(false);
    });
}

void App::SetProcStatus(const std::string& s) {
    std::lock_guard<std::mutex> lk(procMutex);
    m_procStatus = s;
}

void App::QueueProcOutput(const std::string& text, ImU32 color, bool terminalOutput) {
    std::lock_guard<std::mutex> lock(procMutex);
    constexpr size_t queueLimit = 1024 * 1024;
    // Keep the latest output so terminal status is never lost behind a flood.
    std::string bounded = text.size() > queueLimit ? text.substr(text.size() - queueLimit) : text;
    if (bounded.size() != text.size()) outputTruncated = true;
    while (!procOutputs.empty() && queuedOutputBytes + bounded.size() > queueLimit) {
        for (const auto& output : procOutputs.front()) queuedOutputBytes -= output.text.size();
        procOutputs.pop_front(); outputTruncated = true;
    }
    queuedOutputBytes += bounded.size();
    procOutputs.push_back({{std::move(bounded), color, terminalOutput}});
}

std::string App::GetProcStatus() {
    std::lock_guard<std::mutex> lk(procMutex);
    return m_procStatus;
}

void App::PumpProcResults() {
    static bool prevBusy = false;
    std::deque<std::vector<ProcOut>> results;
    bool truncated = false;
    {
        std::lock_guard<std::mutex> lk(procMutex);
        results.swap(procOutputs);
        queuedOutputBytes = 0;
        truncated = outputTruncated; outputTruncated = false;
    }
    for (auto& outs : results)
        for (auto& o : outs) {
            if (o.terminal) {
                terminal.Append(o.text, o.color);
                if (!terminalRevealed) { terminalRevealed = true; showTerminal = true; focusTerminal = true; }
            } else console.Append(o.text, o.color);
        }
    if (truncated) console.Append("[输出] 显示队列已满，部分内容已省略；执行总输出限制仍然生效。\n", IM_COL32(255,190,60,255));
    bool busy = procBusy.load();
    if (busy) { prevBusy = true; return; }        // 运行中, 状态文字由工作线程维护
    if (!results.empty() || prevBusy) SetProcStatus("就绪");
    prevBusy = false;
}

// ===================================================================
//  退出: 等待后台线程结束 (线程持有 this / 引用全局, 不可在运行中销毁)
// ===================================================================
void App::Shutdown() {
    killRequested.store(true);
    shuttingDown.store(true);
    if (procThread.joinable()) procThread.join();
    debugger.Stop(); debugger.Join();
    if (diagThread.joinable()) diagThread.join();
    plugins.Shutdown();
}

// ===================================================================
//  自动保存
// ===================================================================
void App::AutoSaveTick(float dt) {
    autosaveTimer += dt;
    if (autosaveTimer < 3.0f) return;
    autosaveTimer = 0;
    CodeEditor* ed = ActiveEditor();
    if (!ed) return;
    utils::WriteFileU8(dataDir + L"\\autosave.cpp", ed->m_text);
    SaveSession();   // 会话也定期保存, 崩溃后可恢复标签列表
}

void App::SaveSettings() {
    settings.Save(settingsPath, portableMode ? executableDir : std::wstring());
    ApplyTheme();
    console.Append("[设置] 已保存到 " + utils::WToU8(settingsPath) + "\n", IM_COL32(140, 220, 150, 255));
}

// ===================================================================
//  会话恢复: 打开的标签列表 + 光标/滚动位置
// ===================================================================
void App::SaveSession() {
    using json = nlohmann::json;
    try {
        json js;
        js["active"] = active;
        json arr = json::array();
        for (auto& ed : editors) {
            if (!ed->HasPath()) continue;
            json f;
            f["path"]    = utils::WToU8(ed->m_path);
            f["cursor"]  = (unsigned long long)ed->m_cursor;
            f["scrollX"] = ed->m_scrollX;
            f["scrollY"] = ed->m_scrollY;
            f["enc"]     = ed->m_enc;
            f["breakpoints"] = ed->m_breakpoints;
            arr.push_back(f);
        }
        js["files"] = arr;
        utils::WriteFileU8(dataDir + L"\\session.json", js.dump(2));
    } catch (...) {}   // 会话保存失败不影响主流程
}

void App::LoadSession() {
    using json = nlohmann::json;
    std::string s;
    if (utils::ReadFileU8(dataDir + L"\\session.json", s) && !s.empty()) {
        try {
            auto js = json::parse(s);
            int activeIdx = js.value("active", 0);
            if (js.contains("files") && js["files"].is_array() && !js["files"].empty()) {
                for (auto& f : js["files"]) {
                    std::string p = f.value("path", "");
                    if (p.empty()) continue;
                    std::wstring wp = utils::U8ToW(p);
                    if (GetFileAttributesW(wp.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
                    NewFile(wp);
                    if (!editors.empty()) {
                        auto& ed = editors.back();
                        ed->m_cursor = (size_t)f.value("cursor", 0ull);
                        ed->m_selAnchor = ed->m_cursor;
                        ed->m_selActive = false;
                        ed->m_scrollX = f.value("scrollX", 0.0f);
                        ed->m_scrollY = f.value("scrollY", 0.0f);
                        ed->ClampCursor();   // 文件可能被外部改短, 恢复的光标需夹紧到有效范围
                        if(f.contains("breakpoints")&&f["breakpoints"].is_array()) {
                            for(const auto& point:f["breakpoints"]) if(point.is_number_integer()) {
                                int line=point.get<int>();
                                if(line>0 && line<=(int)ed->m_lines.size()) ed->m_breakpoints.push_back(line);
                            }
                            std::sort(ed->m_breakpoints.begin(),ed->m_breakpoints.end());
                            ed->m_breakpoints.erase(std::unique(ed->m_breakpoints.begin(),ed->m_breakpoints.end()),ed->m_breakpoints.end());
                        }
                    }
                }
                if (activeIdx >= 0 && activeIdx < (int)editors.size()) active = activeIdx;
            }
        } catch (...) {}
    }
    if (editors.empty()) NewFile();
    SyncDiagPanel();
    MarkDiag();   // 恢复后对当前文件做一次语法检查
}
