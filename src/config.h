#pragma once
// MiniCPP - 设置 (JSON)
#include <string>
#include <vector>
#include <map>
#include <cstdint>
#include <windows.h>

struct Settings {
    std::string gppPath;          // 手动指定的 g++ 路径 (空 = 自动查找)
    std::string theme = "dark";   // dark / light
    std::string marketUrl;        // 远程插件市场 JSON 地址 (空 = 仅本地)
    int   fontSize = 16;          // 编辑器字体大小
    bool  autoCheck = true;       // 输入停顿后自动语法检查 (类似小熊猫C++)
    bool  codeCompletion = true;  // 补全与参数提示，独立于高亮和语法检查
    bool  syntaxHighlight = true; // 仅控制语法着色，不影响分词、导航和括号匹配
    std::string extraFlags;       // 附加编译参数 (空格分隔, 如 "-g -Wall")

    // ---- 编译运行 (F5) ----
    bool  useCmdTemplate = false; // true=自定义命令行模板; false=标准选项
    int   stdVersion = 0;         // 0=C++17 1=C++20 2=C++14 3=C++11 4=C++23
    int   optLevel = 1;           // 0=无优化 1=-O2 2=-O0 3=-O1 4=-O3 5=-Os
    bool  warnAll = false;        // -Wall -Wextra
    bool  dbgFlag = false;        // -g (调试信息)
    std::string commandTemplate;  // 自定义命令行模板 (空 = 内置默认模板)
    int runTimeLimitMs = 60000;
    int runMemoryLimitMB = 256;    // Job 总提交内存；0 = 不限制
    int runOutputLimitMB = 16;     // stdout/stderr 合计；范围 1..64
    int compileTimeLimitMs = 120000;
    std::string runArguments;       // Windows argv quoting
    bool isolatedRunDirectory=true;
    std::string runDirectory;       // Empty = source directory
    std::string runEnvironment;     // NAME=value, one override per line
    std::string gdbPath;
    std::string debugInputFile;

    // ---- 可配置全局快捷键: 动作名 -> "Ctrl+Shift+S" 组合串 ----
    // 动作名: new/open/save/saveAs/backup/run/nextTab/prevTab
    // 缺省组合在 main.cpp 中定义, 此处仅存用户修改过的项
    std::map<std::string, std::string> hotkeys;

    // ---- 停用的插件 (id 列表) ----
    std::vector<std::string> disabledPlugins;

    bool rainbowBrackets = true;
    bool rainbowIndentLines = false;
    bool rainbowIndentFill = false;
    bool showIndentGuides = true;
    bool showWhitespace = false;
    std::map<std::string, uint32_t> editorColors; // theme/key -> packed RGBA

    // Editor and workspace preferences (v0.3.7.1).
    int tabWidth = 4;
    bool insertSpaces = true;
    bool autoIndent = true;
    bool autoCloseBrackets = true;
    bool autoCompletion = true;
    int completionMinChars = 1;
    int completionRows = 9;
    int completionWidth = 640;
    int autoCheckDelayMs = 800;
    int lineHeightPercent = 125;
    int scrollLines = 3;
    bool showLineNumbers = true;
    bool relativeLineNumbers = false;
    bool highlightCurrentLine = true;
    bool highlightWordOccurrences = true;
    bool showChangeMarkers = true;
    bool caretBlink = true;
    int uiFontSize = 15;
    int uiDensity = 1;
    int cornerRadius = 5;
    int scrollbarSize = 11;
    int recoveryIntervalSec = 3;
    bool showToolbar = true;
    bool showStatusBar = true;
    bool showActivityBar = true;

    void Normalize();
    void ResetPreferences();
    bool ExportPreferences(const std::wstring& path,std::string& error) const;
    bool ImportPreferences(const std::wstring& path,std::string& error);
    bool Load(const std::wstring& path);
    bool Save(const std::wstring& path, const std::wstring& portableRoot = {}) const;
};
