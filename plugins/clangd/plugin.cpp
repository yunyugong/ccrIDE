#include "i18n.h"
#include "plugin_api.h"
#include "session.h"
#include "utils.h"
#include "ui_helpers.h"
#include <imgui.h>
#include <fstream>
#include <filesystem>
#include <shellapi.h>
#include <algorithm>
using clangd_plugin::json;
namespace fs=std::filesystem;
static PluginApiV3* api=nullptr;static clangd_plugin::Session session;
static bool showSettings=false,enabled=true,backgroundIndex=false;
static std::string activeCompiler,observedCompiler;static ULONGLONG compilerChanged=0;static std::string executable,database,flags="-std=c++17",status=i18n::Tr("未启动");
static int memoryMiB=1024;static fs::path settingsPath,moduleDir;
static void Document(const char* file,const char* text,size_t length,unsigned long long revision){session.Document(file,std::string(text,length),revision);}
static void Close(const char* file){session.Close(file);}
static void Request(const char* kind,const char* file,unsigned long long revision,size_t offset){session.Request(kind,file,revision,offset);}
static PluginLanguageHooks hooks{Document,Close,Request};
static void Restart() {
    activeCompiler=api->base.GetSetting("gppPath");observedCompiler=activeCompiler;
    session.Stop();api->RegisterLanguage(nullptr);
    if(!enabled){status=i18n::Tr("已停用，使用核心轻量补全");return;}
    clangd_plugin::Config config;
    if(!executable.empty()){auto path=fs::u8path(executable);config.executable=(path.is_relative()?moduleDir/path:path).wstring();}
    else {
        auto bundled=moduleDir/"toolchain/bin/clangd.exe";
        if(fs::is_regular_file(bundled))config.executable=bundled.wstring();
        else {wchar_t path[32768]{};auto count=SearchPathW(nullptr,L"clangd.exe",nullptr,32768,path,nullptr);if(count&&count<32768)config.executable=path;}
    }
    if(!database.empty()) {
        auto dir=fs::u8path(database);
        if(!fs::is_directory(dir)){status=i18n::Tr("编译数据库目录不存在，请修改设置");return;}
        config.database=fs::absolute(dir).wstring();
    }
    config.compiler=utils::U8ToW(activeCompiler);config.backgroundIndex=backgroundIndex;
    int count=0;auto args=CommandLineToArgvW((L"flags "+utils::U8ToW(flags)).c_str(),&count);
    if(args){for(int i=1;i<count;++i)config.flags.push_back(utils::WToU8(args[i]));LocalFree(args);}
    config.memoryMiB=std::clamp(memoryMiB,128,4096);session.Start(config);api->RegisterLanguage(&hooks);status=i18n::Tr("正在启动 clangd…");
}
static void Open(){showSettings=true;}
static void Save(){try{fs::create_directories(settingsPath.parent_path());std::ofstream(settingsPath)<<json{{"enabled",enabled},{"executable",executable},{"database",database},{"flags",flags},{"memoryMiB",memoryMiB},{"backgroundIndex",backgroundIndex}}.dump(2);Restart();}catch(...){status=i18n::Tr("无法保存或应用配置，请检查目录权限");}}
extern "C" __declspec(dllexport) void Plugin_OnLoadV3(void* context,PluginApi* base) {
    if(!base||base->apiVersion<3||base->structSize<sizeof(PluginApiV3))return;
    api=reinterpret_cast<PluginApiV3*>(base);if(base->GetSetting)i18n::SetLanguage(base->GetSetting("language"));ImGui::SetAllocatorFunctions(base->Allocate,base->Deallocate,base->allocatorUser);ImGui::SetCurrentContext(static_cast<ImGuiContext*>(context));
    static int marker;HMODULE module=nullptr;wchar_t path[32768]{};
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(&marker),&module);GetModuleFileNameW(module,path,32768);moduleDir=fs::path(path).parent_path();
    settingsPath=fs::u8path(base->GetDataDir())/"plugins/clangd/settings.json";
    try {if(fs::is_regular_file(settingsPath)){json config;std::ifstream(settingsPath)>>config;enabled=config.value("enabled",true);executable=config.value("executable",std::string{});database=config.value("database",std::string{});flags=config.value("flags",std::string("-std=c++17"));memoryMiB=config.value("memoryMiB",1024);backgroundIndex=config.value("backgroundIndex",false);}}catch(...){status=i18n::Tr("配置读取失败，使用默认设置");}
    base->RegisterCommand("clangd.settings","clangd 设置",Open);base->RegisterCommand("clangd.restart","重启 clangd",Restart);
    try{Restart();}catch(...){status=i18n::Tr("启动失败，请检查 clangd 设置");}
}
extern "C" __declspec(dllexport) void Plugin_OnFrame() { if(api&&api->base.GetSetting)i18n::SetLanguage((&api->base)->GetSetting("language"));
    if(!api)return;
    std::string compiler=api->base.GetSetting("gppPath");
    if(compiler!=observedCompiler){observedCompiler=compiler;compilerChanged=GetTickCount64();}
    else if(enabled && compiler!=activeCompiler && GetTickCount64()-compilerChanged>800){try{Restart();}catch(...){status=i18n::Tr("编译器变更后重启失败，请检查设置");}}
    for(auto& event:session.Poll()) {
        if(event.value("kind",std::string{})=="status") {status=event.value("message",std::string{});api->base.Log(("[clangd] "+status).c_str());}
        auto text=event.dump();api->PublishLanguage(text.data(),text.size());
    }
    if(!showSettings)return;
    ImGui::SetNextWindowSize(ImVec2(660,460),ImGuiCond_FirstUseEver);
    if(ImGui::Begin(i18n::Window("clangd 设置"),&showSettings)) {
        ImGui::TextWrapped("%s",status.c_str());ImGui::Separator();ImGui::Checkbox(i18n::Tr("启用 clangd 语义服务"),&enabled);
        ImGui::Checkbox(i18n::Tr("工程后台索引（需要编译数据库，可能占用更多内存）"),&backgroundIndex);
        ImGui::TextUnformatted(i18n::Tr("clangd 路径（留空使用随包版本，其次 PATH）"));ImGui::SetNextItemWidth(-FLT_MIN);ui::InputTextStd("##clangd_path",executable);
        ImGui::TextUnformatted(i18n::Tr("compile_commands.json 所在目录（可空，自动发现）"));ImGui::SetNextItemWidth(-FLT_MIN);ui::InputTextStd("##compile_database",database);
        ImGui::TextUnformatted(i18n::Tr("无编译数据库时的参数（支持引号）"));ImGui::SetNextItemWidth(-FLT_MIN);ui::InputTextStd("##fallback_flags",flags);
        ImGui::TextUnformatted(i18n::Tr("clangd 内存上限 MiB（128–4096）"));ImGui::SetNextItemWidth(200);ImGui::InputInt("##memory",&memoryMiB);
        ImGui::Spacing();if(ImGui::Button(i18n::Tr("保存并重启")))Save();ImGui::SameLine();if(ImGui::Button(i18n::Tr("关闭")))showSettings=false;
        ImGui::TextWrapped(i18n::Tr("只向 clangd 查询 IDE 已选择的编译器。工程配置优先使用编译数据库；无数据库时使用上面的参数。未命名文件暂用轻量补全。服务故障会回退，可手动重启。"));
    }
    ImGui::End();
}
extern "C" __declspec(dllexport) void Plugin_OnUnload(){session.Stop();if(api)api->RegisterLanguage(nullptr);api=nullptr;}
