#include "i18n.h"
#include "config.h"
#include "utils.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <type_traits>
#include <stdexcept>
#include "portable_paths.h"

using nlohmann::json;

namespace {
template<class S,class F> void VisitPreferences(S& s,F visit) {
    visit("language",s.language);
    visit("theme",s.theme);
    visit("fontSize",s.fontSize);
    visit("autoCheck",s.autoCheck);
    visit("codeCompletion",s.codeCompletion);
    visit("syntaxHighlight",s.syntaxHighlight);
    visit("hotkeys",s.hotkeys);
    visit("editorColors",s.editorColors);
    visit("rainbowBrackets",s.rainbowBrackets);
    visit("rainbowIndentLines",s.rainbowIndentLines);
    visit("rainbowIndentFill",s.rainbowIndentFill);
    visit("showIndentGuides",s.showIndentGuides);
    visit("showWhitespace",s.showWhitespace);
    visit("tabWidth",s.tabWidth);
    visit("insertSpaces",s.insertSpaces);
    visit("autoIndent",s.autoIndent);
    visit("autoCloseBrackets",s.autoCloseBrackets);
    visit("autoCompletion",s.autoCompletion);
    visit("completionMinChars",s.completionMinChars);
    visit("completionRows",s.completionRows);
    visit("completionWidth",s.completionWidth);
    visit("autoCheckDelayMs",s.autoCheckDelayMs);
    visit("lineHeightPercent",s.lineHeightPercent);
    visit("scrollLines",s.scrollLines);
    visit("showLineNumbers",s.showLineNumbers);
    visit("relativeLineNumbers",s.relativeLineNumbers);
    visit("highlightCurrentLine",s.highlightCurrentLine);
    visit("highlightWordOccurrences",s.highlightWordOccurrences);
    visit("showChangeMarkers",s.showChangeMarkers);
    visit("caretBlink",s.caretBlink);
    visit("uiFontSize",s.uiFontSize);
    visit("uiDensity",s.uiDensity);
    visit("cornerRadius",s.cornerRadius);
    visit("scrollbarSize",s.scrollbarSize);
    visit("recoveryIntervalSec",s.recoveryIntervalSec);
    visit("showToolbar",s.showToolbar);
    visit("showStatusBar",s.showStatusBar);
    visit("showActivityBar",s.showActivityBar);
}
// Reject malformed preference types before applying any changes.
void ReadPreferences(Settings& s,const json& values) {
    if(!values.is_object())throw std::runtime_error(i18n::Tr("偏好内容必须是对象"));
    VisitPreferences(s,[&](const char* key,auto& value){
        if(!values.contains(key))return;
        using T=std::decay_t<decltype(value)>;
        const auto& item=values.at(key);
        if constexpr(std::is_same_v<T,bool>){if(!item.is_boolean())throw std::runtime_error(std::string(key)+i18n::Tr(" 必须为开关"));}
        else if constexpr(std::is_integral_v<T>){if(!item.is_number_integer())throw std::runtime_error(std::string(key)+i18n::Tr(" 必须为整数"));}
        if constexpr(std::is_same_v<T,std::map<std::string,uint32_t>>){
            if(!item.is_object()||item.size()>256)throw std::runtime_error(i18n::Tr("颜色配置无效"));
            for(auto& color:item.items())if(!color.value().is_number_integer()||color.value().get<int64_t>()<0||color.value().get<uint64_t>()>UINT32_MAX)throw std::runtime_error(i18n::Tr("颜色值无效"));
        }
        value=item.get<T>();
    });
    s.Normalize();
}
json PreferenceValues(const Settings& s) {
    json values=json::object();
    VisitPreferences(s,[&](const char* key,const auto& value){values[key]=value;});
    return values;
}
}
void Settings::Normalize() {
    if(language!="en" && language!="zh-CN")language="auto";
    fontSize=std::clamp(fontSize,12,28);
    theme=theme=="light"?"light":"dark";
    tabWidth=std::clamp(tabWidth,2,8);
    completionMinChars=std::clamp(completionMinChars,1,4);
    completionRows=std::clamp(completionRows,4,16);
    completionWidth=std::clamp(completionWidth,360,1000);
    autoCheckDelayMs=std::clamp(autoCheckDelayMs,200,3000);
    lineHeightPercent=std::clamp(lineHeightPercent,110,180);
    scrollLines=std::clamp(scrollLines,1,12);
    uiFontSize=std::clamp(uiFontSize,12,22);
    uiDensity=std::clamp(uiDensity,0,2);
    cornerRadius=std::clamp(cornerRadius,0,12);
    scrollbarSize=std::clamp(scrollbarSize,8,24);
    recoveryIntervalSec=std::clamp(recoveryIntervalSec,1,60);
}

void Settings::ResetPreferences() {
    Settings defaults;
    ReadPreferences(*this,PreferenceValues(defaults));
}
bool Settings::ExportPreferences(const std::wstring& path,std::string& error) const {
    try {
        json data={{"format","ccrIDE.preferences"},{"schema",1},{"preferences",PreferenceValues(*this)}};
        if(!utils::WriteFileU8(path,data.dump(2)))throw std::runtime_error(i18n::Tr("无法写入偏好文件"));
        error.clear();return true;
    } catch(const std::exception& e){error=e.what();return false;}
}
bool Settings::ImportPreferences(const std::wstring& path,std::string& error) {
    try {
        if(utils::FileSize(path)>1024*1024)throw std::runtime_error(i18n::Tr("偏好文件不能超过 1 MiB"));
        std::string text;if(!utils::ReadFileU8(path,text))throw std::runtime_error(i18n::Tr("无法读取偏好文件"));
        auto data=json::parse(text);
        if(data.value("format",std::string{})!="ccrIDE.preferences"||data.value("schema",0)!=1)throw std::runtime_error(i18n::Tr("请选择 ccrIDE 导出的偏好文件"));
        Settings candidate=*this;
        ReadPreferences(candidate,data.at("preferences"));
        *this=std::move(candidate);error.clear();return true;
    } catch(const std::exception& e){error=e.what();return false;}
}

bool Settings::Load(const std::wstring& path) {
    std::string s;
    if (!utils::ReadFileU8(path, s) || s.empty()) return false;
    try {
        json j = json::parse(s);
        ReadPreferences(*this,j);
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
        isolatedRunDirectory=j.value("isolatedRunDirectory",true);
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

        Normalize();
        return true;
    } catch (...) {
        return false;
    }
}

bool Settings::Save(const std::wstring& path, const std::wstring& portableRoot) const {
    // Preserve opaque keys from previous versions for separately installed
    // plugins to migrate later; the host has no knowledge of their schema.
    json j=json::object(); std::string previous;
    if(utils::ReadFileU8(path,previous)) try { auto old=json::parse(previous);if(old.is_object())j=std::move(old); } catch(...) {}
    j.erase("hotkeys");j.erase("disabledPlugins");
    VisitPreferences(*this,[&](const char* key,const auto& value){j[key]=value;});
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
    j["isolatedRunDirectory"]=isolatedRunDirectory;
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
    return utils::WriteFileU8(path, j.dump(2));
}
