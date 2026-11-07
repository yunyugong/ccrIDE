#include "config.h"
#include "utils.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include "portable_paths.h"

using nlohmann::json;

bool Settings::Load(const std::wstring& path) {
    std::string s;
    if (!utils::ReadFileU8(path, s) || s.empty()) return false;
    try {
        json j = json::parse(s);
        if (j.contains("gppPath"))    gppPath    = j["gppPath"].get<std::string>();
        if (j.contains("theme"))      theme      = j["theme"].get<std::string>();
        if (j.contains("marketUrl"))  marketUrl  = j["marketUrl"].get<std::string>();
        if (j.contains("fontSize"))   fontSize   = j["fontSize"].get<int>();
        if (j.contains("autoCheck"))  autoCheck  = j["autoCheck"].get<bool>();
        codeCompletion = j.value("codeCompletion", true);
        syntaxHighlight = j.value("syntaxHighlight", true);
        runTimeLimitMs = std::clamp(j.value("runTimeLimitMs", 60000), 100, 3600000);
        runMemoryLimitMB = std::clamp(j.value("runMemoryLimitMB", 256), 0, 16384);
        runOutputLimitMB = std::clamp(j.value("runOutputLimitMB", 16), 1, 64);
        compileTimeLimitMs = std::clamp(j.value("compileTimeLimitMs", 120000), 100, 3600000);
        runArguments = j.value("runArguments", std::string());
        runDirectory = j.value("runDirectory", std::string());
        runEnvironment = j.value("runEnvironment", std::string());
        gdbPath = j.value("gdbPath", std::string());
        debugInputFile = j.value("debugInputFile", std::string());
        if (j.contains("extraFlags")) extraFlags = j["extraFlags"].get<std::string>();

        // 编译运行选项
        if (j.contains("useCmdTemplate")) useCmdTemplate = j["useCmdTemplate"].get<bool>();
        if (j.contains("stdVersion"))     stdVersion     = j["stdVersion"].get<int>();
        if (j.contains("optLevel"))       optLevel       = j["optLevel"].get<int>();
        if (j.contains("warnAll"))        warnAll        = j["warnAll"].get<bool>();
        if (j.contains("dbgFlag"))        dbgFlag        = j["dbgFlag"].get<bool>();
        if (j.contains("commandTemplate")) commandTemplate = j["commandTemplate"].get<std::string>();

        // 快捷键 (仅存用户修改项; 缺省组合在代码里定义)
        if (j.contains("hotkeys") && j["hotkeys"].is_object()) {
            for (auto it = j["hotkeys"].begin(); it != j["hotkeys"].end(); ++it)
                if (it.value().is_string())
                    hotkeys[it.key()] = it.value().get<std::string>();
        }

        // 停用插件列表
        if (j.contains("disabledPlugins") && j["disabledPlugins"].is_array()) {
            disabledPlugins.clear();
            for (auto& v : j["disabledPlugins"])
                if (v.is_string()) disabledPlugins.push_back(v.get<std::string>());
        }

        if (j.contains("editorColors") && j["editorColors"].is_object()) {
            editorColors.clear();
            for (auto it=j["editorColors"].begin(); it!=j["editorColors"].end(); ++it)
                if (it.value().is_number_unsigned()) editorColors[it.key()] = it.value().get<uint32_t>();
        }
        rainbowBrackets = j.value("rainbowBrackets", true);
        rainbowIndentLines = j.value("rainbowIndentLines", false);
        rainbowIndentFill = j.value("rainbowIndentFill", false);
        showIndentGuides = j.value("showIndentGuides", true);
        showWhitespace = j.value("showWhitespace", false);

        return true;
    } catch (...) {
        return false;
    }
}

void Settings::Save(const std::wstring& path, const std::wstring& portableRoot) const {
    // Preserve opaque keys from previous versions for separately installed
    // plugins to migrate later; the host has no knowledge of their schema.
    json j=json::object(); std::string previous;
    if(utils::ReadFileU8(path,previous)) try { auto old=json::parse(previous);if(old.is_object())j=std::move(old); } catch(...) {}
    j.erase("hotkeys");j.erase("disabledPlugins");
    j["gppPath"]   = portableRoot.empty() ? gppPath : utils::WToU8(portable::Store(portableRoot, utils::U8ToW(gppPath)));
    j["theme"]     = theme;
    j["marketUrl"] = marketUrl;
    j["fontSize"] = fontSize;
    j["autoCheck"] = autoCheck;
    j["codeCompletion"] = codeCompletion;
    j["syntaxHighlight"] = syntaxHighlight;
    j["runTimeLimitMs"] = runTimeLimitMs;
    j["runMemoryLimitMB"] = runMemoryLimitMB;
    j["runOutputLimitMB"] = runOutputLimitMB;
    j["compileTimeLimitMs"] = compileTimeLimitMs;
    j["runArguments"] = runArguments;
    j["runDirectory"] = runDirectory;
    j["runEnvironment"] = runEnvironment;
    j["gdbPath"] = portableRoot.empty() ? gdbPath : utils::WToU8(portable::Store(portableRoot,utils::U8ToW(gdbPath)));
    j["debugInputFile"] = debugInputFile;
    j["extraFlags"] = extraFlags;

    j["useCmdTemplate"] = useCmdTemplate;
    j["stdVersion"]     = stdVersion;
    j["optLevel"]       = optLevel;
    j["warnAll"]        = warnAll;
    j["dbgFlag"]        = dbgFlag;
    j["commandTemplate"] = commandTemplate;

    if (!hotkeys.empty()) {
        json h = json::object();
        for (auto& kv : hotkeys) h[kv.first] = kv.second;
        j["hotkeys"] = h;
    }

    if (!disabledPlugins.empty()) {
        json arr = json::array();
        for (auto& id : disabledPlugins) arr.push_back(id);
        j["disabledPlugins"] = arr;
    }

    j["editorColors"] = editorColors;
    j["rainbowBrackets"] = rainbowBrackets;
    j["rainbowIndentLines"] = rainbowIndentLines;
    j["rainbowIndentFill"] = rainbowIndentFill;
    j["showIndentGuides"] = showIndentGuides;
    j["showWhitespace"] = showWhitespace;
    utils::WriteFileU8(path, j.dump(2));
}
