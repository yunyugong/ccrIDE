#include "plugin_manager.h"
#include "app.h"
#include "utils.h"
#include "ui_helpers.h"
#include "zip_archive.h"
#include "language_host.h"
#include "plugin_validation.h"
#include <map>
#include <imgui.h>
#include <imgui_internal.h>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <algorithm>
#include <vector>
#include <cwctype>

using nlohmann::json;
namespace fs = std::filesystem;

static std::wstring PluginRoot() { return g_app.pluginDir.empty() ? utils::GetExeDir()+L"\\plugins" : g_app.pluginDir; }

static bool ValidPluginId(const std::string& id) {
    if(id.empty() || id.size()>80 || !std::all_of(id.begin(),id.end(),[](unsigned char c){
        return (c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') || c=='-' || c=='_';
    })) return false;
    try {packagezip::SafeName(id,true);return true;}catch(...){return false;}
}
static bool ValidPluginEntry(const std::string& entry) {
    if(entry.empty() || entry.find_first_of("/\\")!=std::string::npos)return false;
    try {
        auto path=fs::path(packagezip::SafeName(entry,true));auto extension=path.extension().wstring();
        std::transform(extension.begin(),extension.end(),extension.begin(),towlower);
        return extension==L".dll";
    }catch(...){return false;}
}

// ---------------- 宿主 API 实现 ----------------
static const char* api_get_data_dir() { static std::string s; s = utils::WToU8(g_app.dataDir); return s.c_str(); }
static void        api_log(const char* msg) { g_app.console.Append(msg, IM_COL32(140, 180, 255, 255)); g_app.console.Append("\n", IM_COL32(255,255,255,255)); }
static const char* api_get_setting(const char* key) {
    static std::string s;
    if (!strcmp(key, "gppPath"))   s = g_app.settings.gppPath;
    else if (!strcmp(key, "theme")) s = g_app.settings.theme;
    else if (!strcmp(key, "marketUrl")) s = g_app.settings.marketUrl;
    else s.clear();
    return s.c_str();
}

struct PluginCommand { std::string id,label; void (*execute)(); };
static std::vector<PluginCommand> g_commands;
static PluginCompletionHooks g_completion{};
static unsigned int g_docks[3]{};
static void api_register_command(const char* id,const char* label,void (*execute)()) {
    if(!id || !label || !execute) return;
    for(auto& command:g_commands) if(command.id==id) { command={id,label,execute}; return; }
    g_commands.push_back({id,label,execute});
}
static void api_register_completion(const PluginCompletionHooks* hooks) { g_completion=hooks ? *hooks : PluginCompletionHooks{}; }
static void api_selection(PluginTextSink sink,void* user) { auto* editor=g_app.ActiveEditor(); if(editor && sink) { auto text=editor->GetSelectionText(); sink(user,text.data(),text.size()); } }
static void api_insert(const char* text) { if(auto* editor=g_app.ActiveEditor()) if(text) editor->InsertText(text); }
static void api_dock(const char* title,int location) {
    if(!title || location<0 || location>2 || !g_docks[location])return;
    if(ImGui::FindWindowByName(title) || ImGui::FindWindowSettingsByID(ImHashStr(title)))return;
    auto* node=ImGui::DockBuilderGetNode(g_docks[location]);
    if(location==2 && node && node->IsSplitNode()) {
        auto* center=node->CentralNode ? node->CentralNode : node;
        ImGuiID right=0,remainder=0;
        ImGui::DockBuilderSplitNode(center->ID,ImGuiDir_Right,0.30f,&right,&remainder);
        ImGui::DockBuilderFinish(node->ID);g_docks[location]=right;
    }
    ImGui::SetNextWindowDockID(g_docks[location],ImGuiCond_FirstUseEver);
}
static PluginApiV3 g_apiV3 = {{ api_get_data_dir,api_log,api_get_setting,CCRIDE_PLUGIN_API_VERSION,sizeof(PluginApiV3),api_register_command,api_register_completion,api_selection,api_insert,api_dock },language::Register,language::Publish};
static PluginApi& g_pluginApi=g_apiV3.base;

bool PluginManager::HasCompletionProvider() const { return g_completion.Describe && g_completion.Draw; }
void PluginManager::BeginCompletion() { if(g_completion.Begin) g_completion.Begin(); }
std::string PluginManager::CompletionDocumentation(const std::string& text,size_t offset) {
    std::string result;
    if(g_completion.Describe) g_completion.Describe(text.data(),text.size(),offset,[](void* user,const char* value,size_t size) { static_cast<std::string*>(user)->assign(value,size); },&result);
    return result;
}
void PluginManager::DrawCompletionDetails(CodeEditor& editor) {
    if(!HasCompletionProvider() || editor.m_compIdx<0 || editor.m_compIdx>=(int)editor.m_compItems.size()) return;
    auto& item=editor.m_compItems[editor.m_compIdx];
    std::vector<std::string> files; for(auto& source:item.sources) files.push_back(utils::WToU8(source.file));
    std::vector<PluginCompletionSource> sources;
    for(size_t i=0;i<item.sources.size();++i) sources.push_back({files[i].c_str(),item.sources[i].line,item.sources[i].doc.c_str()});
    PluginCompletionView view{item.label.c_str(),item.hint.c_str(),sources.data(),sources.size(),&item.sourceIndex};
    g_completion.Draw(&view);
}
void PluginManager::DrawCommands() { for(const auto& command:g_commands) if(ImGui::MenuItem(command.label.c_str())) command.execute(); }
void PluginManager::SetDockTargets(unsigned int left,unsigned int bottom,unsigned int right) { g_docks[0]=left;g_docks[1]=bottom;g_docks[2]=right; }


// ---------------- 扫描与加载 ----------------
void PluginManager::ScanAndLoad() {
    UnloadAll();
    plugins.clear();
    ImGui::GetAllocatorFunctions(&g_pluginApi.Allocate,&g_pluginApi.Deallocate,&g_pluginApi.allocatorUser);

    std::wstring root = PluginRoot();
    if (GetFileAttributesW(root.c_str()) == INVALID_FILE_ATTRIBUTES) return;

    auto isDisabled = [](const std::string& id) {
        for (auto& d : g_app.settings.disabledPlugins)
            if (d == id) return true;
        return false;
    };

    auto key=[](std::string id){for(char& c:id)if(c>='A'&&c<='Z')c=char(c-'A'+'a');return id;};
    std::map<std::string,size_t> identities;
    std::error_code scanError;
    for(fs::directory_iterator it(fs::path(root),scanError),end;!scanError&&it!=end;it.increment(scanError)){
        auto dir=it->path();DWORD attr=GetFileAttributesW(dir.c_str());
        if(attr==INVALID_FILE_ATTRIBUTES||!(attr&FILE_ATTRIBUTE_DIRECTORY)||(attr&FILE_ATTRIBUTE_REPARSE_POINT))continue;
        std::string manifest;if(utils::ReadFileU8((dir/L"plugin.json").wstring(),manifest))try{auto id=json::parse(manifest).value("id","");if(ValidPluginId(id))++identities[key(id)];}catch(...){}
    }
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((root + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring name = fd.cFileName;
        if (name == L"." || name == L".." || !(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        std::wstring dir = root + L"\\" + name;
        std::wstring manifestPath = dir + L"\\plugin.json";
        std::string manifestText;
        if (!utils::ReadFileU8(manifestPath, manifestText)) continue;

        PluginInfo pi;
        unsigned coordinator=0;
        pi.dir = dir;
        try {
            json j = json::parse(manifestText);
            pi.id          = j.value("id", "");
            pi.name        = j.value("name", utils::WToU8(name));
            pi.version     = j.value("version", "1.0");
            pi.author      = j.value("author", "");
            pi.description = j.value("description", "");
            pi.entry       = j.value("entry", "");
            pi.apiVersion  = j.value("apiVersion",1u);
            coordinator = j.value("taskCoordinatorVersion",0u);
            pi.kind        = j.value("kind", "soft");   // 默认 soft=外置 DLL
            pi.bundled     = j.value("bundled", false);
        } catch (...) { continue; }
        if (!ValidPluginId(pi.id)) continue;
        pi.enabled = !isDisabled(pi.id);

        if (!pi.enabled) {
            // 用户停用 -> 只记录不加载, 便于在插件管理中重新启用
            plugins.push_back(std::move(pi));
            continue;
        }

        if(identities[key(pi.id)]>1) {
            pi.loadError="多个插件声明相同 ID，已阻止加载，请移除重复目录";
        } else if((pi.id=="ai-assistant" || pi.id=="clangd") && coordinator!=1) {
            pi.loadError="此服务插件未接入任务调度，请更新到 1.2.0 或更高兼容版本";
        } else if(pi.apiVersion<1 || pi.apiVersion>CCRIDE_PLUGIN_API_VERSION) {
            pi.loadError="插件需要接口版本 "+std::to_string(pi.apiVersion)+"，当前支持 1–"+std::to_string(CCRIDE_PLUGIN_API_VERSION);
        } else if(pi.kind!="hard" && pi.kind!="soft") {
            pi.loadError="未知的插件类型";
        } else if(pi.kind=="soft" && !ValidPluginEntry(pi.entry)) {
            pi.loadError="插件清单中的 DLL 文件名无效";
        }
        if(!pi.loadError.empty()) {
            g_app.console.Append("[插件] "+pi.name+": "+pi.loadError+"\n",IM_COL32(255,160,100,255));
            plugins.push_back(std::move(pi));continue;
        }

        if (pi.kind == "hard") {
            // 硬编译插件: 源码已随主程序编译链接, 用注册表按 id 匹配入口
            const PluginFns* f = builtinLookup ? builtinLookup(pi.id.c_str()) : nullptr;
            if (!f || !f->onLoad) {
                pi.loadError="内置插件未编入当前程序";
                g_app.console.Append("[插件] 硬编译插件未编入当前程序: " + pi.name +
                                     " (需要重新运行 build.ps1 构建 IDE)\n",
                                     IM_COL32(255, 190, 60, 255));
            } else {
                pi.onLoad   = f->onLoad;
                pi.onFrame  = f->onFrame;
                pi.onUnload = f->onUnload;
                pi.loaded   = true;
                pi.onLoad(ImGui::GetCurrentContext(), &g_pluginApi);
                g_app.console.Append("[插件] 已加载: " + pi.name + " v" + pi.version +
                                     " [硬编译]\n", IM_COL32(120, 220, 140, 255));
            }
        } else {
            // 软编译插件: 外部 DLL 动态加载
            if (pi.entry.empty()) {
                plugins.push_back(std::move(pi));   // 尚无 entry (未安装 DLL)
                continue;
            }
            pi.dllPath = fs::absolute(fs::path(dir)/utils::U8ToW(pi.entry)).wstring();
            if (pluginvalidation::NativeDll(pi.dllPath,pi.loadError)) {
                HMODULE hmod = LoadLibraryExW(pi.dllPath.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
                if (hmod) {
                    pi.hmod    = hmod;
                    pi.onLoad  = (Plugin_OnLoadFn)GetProcAddress(hmod, pi.apiVersion>=3 ? "Plugin_OnLoadV3" : (pi.apiVersion>=2 ? "Plugin_OnLoadV2" : PLUGIN_LOAD_NAME));
                    pi.onFrame = (Plugin_OnFrameFn)GetProcAddress(hmod, PLUGIN_FRAME_NAME);
                    pi.onUnload= (Plugin_OnUnloadFn)GetProcAddress(hmod, PLUGIN_UNLOAD_NAME);
                    if (pi.onLoad) {
                        pi.loaded = true;
                        pi.onLoad(ImGui::GetCurrentContext(), &g_pluginApi);
                        g_app.console.Append("[插件] 已加载: " + pi.name + " v" + pi.version +
                                             " [DLL]\n", IM_COL32(120, 220, 140, 255));
                    } else {
                        pi.loadError="DLL 缺少加载入口，请检查插件接口版本";
                        FreeLibrary(hmod);pi.hmod=nullptr;pi.onFrame=nullptr;pi.onUnload=nullptr;
                    }
                } else {
                    pi.loadError="无法加载 DLL（系统错误 "+std::to_string(GetLastError())+"），请检查依赖或 64 位兼容性";
                }
            }
        }
        if(!pi.loadError.empty())g_app.console.Append("[插件] "+pi.name+": "+pi.loadError+"\n",IM_COL32(255,160,100,255));
        plugins.push_back(std::move(pi));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

void PluginManager::UnloadAll() {
    language::Reset();
    g_commands.clear(); g_completion={};
    for(auto& editor:g_app.editors) editor->CloseCompletion();
    for (auto& p : plugins) {
        if (p.loaded && p.onUnload) p.onUnload();   // 硬/软都调用卸载回调
        if (p.hmod) FreeLibrary(p.hmod);
    }
    plugins.clear();
}

void PluginManager::ReloadAll() {
    UnloadAll();
    ScanAndLoad();
}

void PluginManager::CallFrames() {
    language::Tick();
    for (auto& p : plugins) {
        if (!p.loaded || !p.onFrame) continue;
        p.onFrame();
    }
}

// ---------------- 市场 ----------------
void PluginManager::LoadBundledCatalog() {
    static const char* bundledJson = R"([
      {"id":"clock","name":"时钟","version":"1.0","author":"ccrIDE","description":"实时时钟示例插件","entry":"clock.dll","bundled":true},
      {"id":"hello","name":"示例插件","version":"1.0","author":"ccrIDE","description":"插件 API 最小示例","entry":"hello.dll","bundled":true}
    ])";
    try {
        for (auto& j : json::parse(bundledJson)) {
            MarketItem it;
            it.id          = j.value("id", "");
            it.name        = j.value("name", "");
            it.version     = j.value("version", "");
            it.author      = j.value("author", "");
            it.description = j.value("description", "");
            it.entry       = j.value("entry", "");
            it.bundled     = j.value("bundled", false);
            catalog.push_back(std::move(it));
        }
    } catch (...) {}
    catalogLoaded = true;
    RefreshInstalledFlags();
}

// ---------------- 市场 (网络在后台线程执行, 结果经消息队列回主线程) ----------------
void PluginManager::PushMsg(NetKind kind, std::string text, std::vector<MarketItem> items) {
    std::lock_guard<std::mutex> lk(m_resMtx);
    m_msgs.push_back({ kind, std::move(text), std::move(items) });
}

void PluginManager::RequestFetchRemote() {
    if (m_netBusy.exchange(true)) {
        g_app.console.Append("[市场] 已有网络任务进行中\n", IM_COL32(255, 200, 100, 255));
        return;
    }
    if (g_app.settings.marketUrl.empty()) {
        m_netBusy.store(false);
        lastMarketError = "未配置线上市场地址 (请在 设置 中填写 marketUrl)";
        return;
    }
    std::string url = g_app.settings.marketUrl;   // 值拷贝, 线程内不读共享设置
    if (m_worker.joinable()) m_worker.join();
    m_worker = std::thread([this, url]() {
        std::string body, err;
        if (!utils::HttpGet(url, body, err)) {
            PushMsg(NetKind::FetchErr, err);
        } else {
            try {
                std::vector<MarketItem> items;
                for (auto& j : json::parse(body)) {
                    MarketItem it;
                    it.id          = j.value("id", "");
                    it.name        = j.value("name", "");
                    it.version     = j.value("version", "");
                    it.author      = j.value("author", "");
                    it.description = j.value("description", "");
                    it.entry       = j.value("entry", "");
                    it.dllUrl      = j.value("dllUrl", "");
                    it.sha256      = j.value("sha256", "");
                    it.apiVersion  = j.value("apiVersion",1u);
                    it.taskCoordinatorVersion=j.value("taskCoordinatorVersion",0u);
                    it.bundled     = false;
                    if (!ValidPluginId(it.id) || !ValidPluginEntry(it.entry) || it.dllUrl.empty() || it.apiVersion<1 || it.apiVersion>CCRIDE_PLUGIN_API_VERSION) continue;
                    items.push_back(std::move(it));
                }
                if (items.empty()) PushMsg(NetKind::FetchErr, "线上市场返回空列表");
                else PushMsg(NetKind::FetchOk, "", std::move(items));
            } catch (...) {
                PushMsg(NetKind::FetchErr, "线上市场 JSON 解析失败");
            }
        }
        m_netBusy.store(false);
    });
}

void PluginManager::RequestInstall(const MarketItem& it) {
    if(!ValidPluginId(it.id) || !ValidPluginEntry(it.entry) || it.apiVersion<1 || it.apiVersion>CCRIDE_PLUGIN_API_VERSION || it.dllUrl.empty()) {
        PushMsg(NetKind::InstallFail,"插件清单包含无效的路径或不兼容的接口版本");return;
    }
    if (m_netBusy.exchange(true)) {
        g_app.console.Append("[市场] 已有网络任务进行中\n", IM_COL32(255, 200, 100, 255));return;
    }
    MarketItem copy=it;
    fs::path root=PluginRoot();
    if (m_worker.joinable()) m_worker.join();
    m_worker=std::thread([this,copy,root]() {
        fs::path staging;bool owned=false;
        try {
            fs::path target=root/fs::u8path(copy.id);
            if(fs::exists(target))throw std::runtime_error("该插件已存在，请先卸载旧版");
            std::string dllData,error;
            if(!utils::HttpGet(copy.dllUrl,dllData,error))throw std::runtime_error("下载失败: "+error);
            if(!copy.sha256.empty()) {
                std::string expected=copy.sha256;
                for(auto& c:expected)c=(char)tolower((unsigned char)c);
                if(utils::Sha256Hex(dllData)!=expected)throw std::runtime_error("SHA-256 校验失败");
            }
            fs::create_directories(root);
            staging=root/(L".install-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
            owned=fs::create_directory(staging);
            if(!owned)throw std::runtime_error("无法创建插件临时目录");
            if(!utils::WriteFileU8((staging/fs::u8path(copy.entry)).wstring(),dllData))throw std::runtime_error("写入 DLL 失败");
            json manifest={{"id",copy.id},{"name",copy.name},{"version",copy.version},
                {"author",copy.author},{"description",copy.description},{"entry",copy.entry},
                {"taskCoordinatorVersion",copy.taskCoordinatorVersion},{"apiVersion",copy.apiVersion},{"kind","soft"},{"bundled",false}};
            if(!utils::WriteFileU8((staging/L"plugin.json").wstring(),manifest.dump(2)))throw std::runtime_error("写入插件清单失败");
            fs::rename(staging,target);owned=false;
            PushMsg(NetKind::InstallOk,copy.name);
        }catch(const std::exception& error) {
            if(owned)packagezip::RemoveStagingTree(staging);
            PushMsg(NetKind::InstallFail,error.what());
        }
        m_netBusy.store(false);
    });
}

// 主线程每帧消费网络结果: 共享数据 (catalog / lastMarketError / console) 只在此处改动
void PluginManager::PumpNetResults() {
    std::deque<NetMsg> msgs;
    { std::lock_guard<std::mutex> lk(m_resMtx); msgs.swap(m_msgs); }
    for (auto& m : msgs) {
        switch (m.kind) {
            case NetKind::FetchErr:
                lastMarketError = m.text;
                break;
            case NetKind::FetchOk: {
                lastMarketError.clear();
                int added = 0;
                for (auto& it : m.items) {
                    bool dup = false;
                    for (auto& c : catalog) if (c.id == it.id) { dup = true; break; }
                    if (!dup) { catalog.push_back(std::move(it)); added++; }
                }
                RefreshInstalledFlags();
                g_app.console.Append("[市场] 线上市场已刷新 (新增 " + std::to_string(added) + " 个条目)\n",
                                     IM_COL32(120, 220, 140, 255));
                break;
            }
            case NetKind::InstallOk:
                RefreshInstalledFlags();
                g_app.console.Append("[市场] 已安装: " + m.text + ", 点击 [重载插件] 生效\n",
                                     IM_COL32(120, 220, 140, 255));
                break;
            case NetKind::InstallFail:
                g_app.console.Append("[市场] 安装失败: " + m.text + "\n",
                                     IM_COL32(255, 120, 120, 255));
                break;
        }
    }
}

void PluginManager::Shutdown() {
    if (m_worker.joinable()) m_worker.join();
}

void PluginManager::RefreshInstalledFlags() {
    std::wstring root = PluginRoot();
    for (auto& it : catalog) {
        std::wstring dir = root + L"\\" + utils::U8ToW(it.id);
        it.installed = (GetFileAttributesW(dir.c_str()) != INVALID_FILE_ATTRIBUTES);
    }
}

bool PluginManager::Uninstall(const std::string& id, std::string& err) {
    if(!ValidPluginId(id)) {err="无效的插件 ID";return false;}
    if(m_netBusy.load()) {err="请等待当前插件下载完成";return false;}
    std::wstring dir = PluginRoot() + L"\\" + utils::U8ToW(id);
    if (GetFileAttributesW(dir.c_str()) == INVALID_FILE_ATTRIBUTES) { err = "未安装"; return false; }
    // Keep DLLs unloaded until their files are deleted; reloading before deletion
    // would lock them again and prevent an actual uninstall on Windows.
    UnloadAll();
    std::error_code ec;
    fs::remove_all(dir, ec);
    ScanAndLoad();
    if (ec) { err = "删除目录失败: " + ec.message(); return false; }
    RefreshInstalledFlags();
    return true;
}

// ---------------- 市场 UI ----------------
void PluginManager::DrawMarketWindow(bool* open) {
    if (!ImGui::Begin("插件市场", open)) { ImGui::End(); return; }

    PumpNetResults();   // 消费后台网络结果

    ImGui::TextWrapped("插件目录: %s。安装插件后点击 [重载插件] 生效。", utils::WToU8(PluginRoot()).c_str());

    if (ImGui::Button("刷新线上市场")) RequestFetchRemote();
    ImGui::SameLine();
    if (ImGui::Button("重载插件")) ReloadAll();
    ImGui::SameLine();
    if (ImGui::Button("打开插件目录")) {
        ShellExecuteW(nullptr, L"open", PluginRoot().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    if (m_netBusy.load())
        ImGui::TextColored(ImVec4(1, 0.85f, 0.4f, 1), "网络任务进行中...");
    if (!lastMarketError.empty())
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", lastMarketError.c_str());

    ImGui::Separator();

    static ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                   ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
    ImVec2 avail = ImGui::GetContentRegionAvail();
    if (ImGui::BeginTable("market", 4, flags, ImVec2(avail.x, avail.y - 8))) {
        ImGui::TableSetupColumn("名称", ImGuiTableColumnFlags_WidthFixed, 110);
        ImGui::TableSetupColumn("版本", ImGuiTableColumnFlags_WidthFixed, 60);
        ImGui::TableSetupColumn("描述", 0, 0);
        ImGui::TableSetupColumn("操作", ImGuiTableColumnFlags_WidthFixed, 90);
        ImGui::TableHeadersRow();

        for (auto& it : catalog) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            std::string status = it.installed ? " [已安装]" : (it.bundled ? " [随附]" : "");
            ImGui::TextUnformatted((it.name + status).c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(it.version.c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetColumnWidth() - 8);
            ImGui::TextUnformatted(it.description.c_str());
            ImGui::PopTextWrapPos();
            ImGui::TableSetColumnIndex(3);
            std::string btnId;
            if (it.installed) {
                btnId = "卸载##" + it.id;
                if (ImGui::Button(btnId.c_str())) {
                    std::string err;
                    if (!Uninstall(it.id, err))
                        g_app.console.Append("[市场] 卸载失败: " + err + "\n", IM_COL32(255, 120, 120, 255));
                }
            } else {
                btnId = "安装##" + it.id;
                if (ImGui::Button(btnId.c_str())) {
                    if (it.bundled) {
                        // 随附插件由构建脚本放入插件目录, 未出现则提示
                        g_app.console.Append("[市场] 随附插件需重新构建 IDE (build.ps1) 生成\n", IM_COL32(255, 200, 100, 255));
                    } else {
                        RequestInstall(it);
                    }
                }
            }
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

// ===================================================================
//  软编译插件源码 -> DLL (编译入口, 同步执行)
// ===================================================================
// DLL 链接自己的 ImGui 副本，但共享宿主上下文与进程，不提供故障或权限隔离。
static bool CompileSoftPluginDll(const std::wstring& src, const std::wstring& extraInc,
                                 std::string& err) {
    std::wstring gpp = utils::U8ToW(g_app.settings.gppPath);
    if (gpp.empty()) { err = "未配置 g++ 路径 (设置 -> 编译器)"; return false; }
    if (GetFileAttributesW(src.c_str()) == INVALID_FILE_ATTRIBUTES) {
        err = "找不到源文件: " + utils::WToU8(src); return false;
    }
    std::wstring dir = utils::DirName(src);
    std::wstring outDll = utils::ReplaceExt(src, L"dll");
    std::wstring sdk = g_app.executableDir + L"\\sdk";

    std::vector<std::wstring> args = {
        src, L"-std=c++17", L"-O2", L"-shared",
        L"-static-libgcc", L"-static-libstdc++",
        L"-I" + sdk + L"\\imgui", L"-I" + sdk + L"\\imgui\\backends", L"-I" + sdk + L"\\nlohmann",
        L"-I" + dir,
    };
    // IDE 源码目录 (含 plugin_api.h): 开发版布局 exe 在 build 下, 源码在其上级 src
    wchar_t exeBuf[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, exeBuf, MAX_PATH);
    std::wstring ideDir = utils::DirName(exeBuf);
    ideDir += L"\\..\\src";
    if (GetFileAttributesW((sdk + L"\\include").c_str()) != INVALID_FILE_ATTRIBUTES) ideDir = sdk + L"\\include";
    if (GetFileAttributesW(ideDir.c_str()) != INVALID_FILE_ATTRIBUTES)
        args.push_back(L"-I" + ideDir);
    // 用户附加 -I 目录 (空格分隔)
    if (!extraInc.empty()) {
        std::wstring ef = extraInc;
        size_t pos = 0;
        while (pos < ef.size()) {
            while (pos < ef.size() && iswspace(ef[pos])) pos++;
            size_t e = pos;
            while (e < ef.size() && !iswspace(ef[e])) e++;
            if (e > pos) args.push_back(L"-I" + ef.substr(pos, e - pos));
            pos = e;
        }
    }
    // 插件链接自己的 ImGui 核心函数，但访问同一宿主上下文，必须保持 ABI 和分配器一致。
    for (const wchar_t* f : {L"imgui.cpp", L"imgui_draw.cpp", L"imgui_tables.cpp", L"imgui_widgets.cpp"})
        args.push_back(sdk + L"\\imgui\\" + f);
    args.push_back(L"-o");
    args.push_back(outDll);

    auto r = utils::RunProcess(gpp, args, dir,
                               L"", L"", 60000, true,nullptr,execution::Lane::Foreground,"插件源码编译");
    if (!r.ok) { err = "无法启动编译器: " + r.error; return false; }
    if (r.exitCode != 0) {
        err = r.err.empty() ? ("编译失败 (退出码 " + std::to_string(r.exitCode) + ")") : r.err;
        return false;
    }
    g_app.console.Append("[插件] 编译成功: " + utils::WToU8(outDll) + "\n",
                         IM_COL32(120, 220, 140, 255));
    g_app.console.Append("[插件] 提示: 产物 DLL 名需与 plugin.json 的 entry 字段一致才能被加载\n",
                         IM_COL32(180, 180, 190, 255));
    return true;
}

void PluginManager::TogglePluginEnabled(const std::string& id) {
    auto& dis = g_app.settings.disabledPlugins;
    bool nowDisabled = (std::find(dis.begin(), dis.end(), id) == dis.end());
    if (nowDisabled) dis.push_back(id);
    else dis.erase(std::remove(dis.begin(), dis.end(), id), dis.end());
    g_app.SaveSettings();
    g_app.console.Append(nowDisabled ? ("[插件] 已停用 (重载后不再加载): " + id + "\n")
                                     : ("[插件] 已启用: " + id + "\n"),
                         nowDisabled ? IM_COL32(255, 200, 100, 255) : IM_COL32(120, 220, 140, 255));
    ReloadAll();
}

// ---------------- 插件管理窗口 ----------------
void PluginManager::DrawPluginsWindow(bool* open) {
    if (!ImGui::Begin("插件管理", open)) { ImGui::End(); return; }

    ImGui::TextWrapped("硬编译 = 随 ccrIDE 一起构建 (无 DLL); DLL = 外置插件。"
                       "停用插件后其窗口将不再出现。");
    if (ImGui::Button("重载插件")) ReloadAll();
    ImGui::SameLine();
    if (ImGui::Button("打开插件目录")) {
        ShellExecuteW(nullptr, L"open", PluginRoot().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    ImGui::SameLine();
    if (ImGui::Button("编译软插件 (源码 → DLL)")) showCompile = true;

    ImGui::Separator();

    static ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                   ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
    ImVec2 avail = ImGui::GetContentRegionAvail();
    if (ImGui::BeginTable("plugins", 5, flags, ImVec2(avail.x, avail.y - 8))) {
        ImGui::TableSetupColumn("名称", ImGuiTableColumnFlags_WidthFixed, 150);
        ImGui::TableSetupColumn("类型", ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("版本", ImGuiTableColumnFlags_WidthFixed, 50);
        ImGui::TableSetupColumn("状态", ImGuiTableColumnFlags_WidthFixed, 90);
        ImGui::TableSetupColumn("操作", ImGuiTableColumnFlags_WidthFixed, 100);
        ImGui::TableHeadersRow();

        std::string toggleId,uninstallId;
        for (auto& p : plugins) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(p.name.c_str());
            ImGui::TableSetColumnIndex(1);
            ImVec4 c = (p.kind == "hard") ? ImVec4(0.6f, 0.8f, 1.0f, 1.0f) : ImVec4(1.0f, 0.8f, 0.6f, 1.0f);
            ImGui::TextColored(c, "%s", p.kind == "hard" ? "硬编译" : "DLL");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(p.kind == "hard"
                    ? "源码随主程序编译链接 (plugin.json 的 kind=hard)"
                    : "外部 DLL 动态加载 (plugin.json 的 kind=soft)");
            }
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(p.version.c_str());
            ImGui::TableSetColumnIndex(3);
            if (!p.enabled) {
                ImGui::TextColored(ImVec4(1, 0.85f, 0.4f, 1), "已停用");
            } else if (p.loaded) {
                ImGui::TextColored(ImVec4(0.5f, 0.85f, 0.55f, 1), "运行中");
            } else {
                ImGui::TextColored(ImVec4(1, 0.65f, 0.4f, 1), "加载失败");
                if(!p.loadError.empty() && ImGui::IsItemHovered())ImGui::SetTooltip("%s",p.loadError.c_str());
            }
            ImGui::TableSetColumnIndex(4);
            ImGui::PushID(p.id.c_str());
            std::string btn = p.enabled ? "停用##a" : "启用##a";
            if (ImGui::Button(btn.c_str(), ImVec2(80, 0))) toggleId = p.id;
            if(p.kind=="soft" && ImGui::Button("卸载",ImVec2(80,0))) uninstallId=p.id;
            ImGui::PopID();
        }
        ImGui::EndTable();
        if(!uninstallId.empty()) {
            std::string error;if(!Uninstall(uninstallId,error))g_app.console.Append("[插件] 卸载失败: "+error+"\n",IM_COL32(255,120,120,255));
        } else if (!toggleId.empty()) TogglePluginEnabled(toggleId);   // 循环外处理, 避免迭代失效
    }

    // 编译软插件弹窗
    if (showCompile) ImGui::OpenPopup("编译软插件");
    if (ImGui::BeginPopupModal("编译软插件", &showCompile, ImGuiWindowFlags_AlwaysAutoResize)) {
        static std::string srcPath, extraInc;
        ImGui::TextWrapped("选择要编译成 DLL 的插件源文件 (.cpp)。产物写到同目录, DLL 名=源文件名。");
        ui::InputTextStd("源文件 (.cpp)", srcPath);
        if (ImGui::Button("浏览...")) {
            std::wstring p;
            if (utils::OpenFileDialog(g_app.dataDir, p)) srcPath = utils::WToU8(p);
        }
        ui::InputTextStd("附加 -I 目录 (可选)", extraInc);
        bool doCompile = ImGui::Button("开始编译", ImVec2(120, 0));
        ImGui::SameLine();
        if (ImGui::Button("关闭", ImVec2(120, 0))) showCompile = false;
        if (doCompile) {
            if (srcPath.empty()) {
                g_app.console.Append("[插件] 请先选择源文件\n", IM_COL32(255, 200, 100, 255));
            } else {
                UnloadAll();   // 若目标 DLL 正被加载会占用文件, 先全部卸载
                std::string err;
                bool ok = CompileSoftPluginDll(utils::U8ToW(srcPath), utils::U8ToW(extraInc), err);
                if (!ok)
                    g_app.console.Append("[插件] 编译失败: " + err + "\n", IM_COL32(255, 120, 120, 255));
                ScanAndLoad();   // 重新加载 (新的 DLL / 原插件)
            }
        }
        ImGui::EndPopup();
    }
    ImGui::End();
}

bool PluginManager::InstallLocalArchive(const std::wstring& archive,std::string& error) {
    fs::path staging;bool owned=false;
    struct Cleanup {fs::path& path;bool& owned;~Cleanup(){if(owned)packagezip::RemoveStagingTree(path);}} cleanup{staging,owned};
    try {
        error.clear();
        fs::path root=PluginRoot();fs::create_directories(root);
        staging=root/(L".install-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
        owned=fs::create_directory(staging);
        if(!owned)throw std::runtime_error("无法建立插件临时目录");
        if(!packagezip::Extract(archive,0,fs::file_size(archive),staging,error,{},128ull<<20)) return false;
        fs::path source;
        for(const auto& entry:fs::directory_iterator(staging)) {
            if(!entry.is_directory() || !source.empty()) throw std::runtime_error("插件包必须仅包含一个插件文件夹");
            source=entry.path();
        }
        if(source.empty()) throw std::runtime_error("插件包为空");
        std::string text;if(!utils::ReadFileU8((source/L"plugin.json").wstring(),text)) throw std::runtime_error("找不到 plugin.json");
        auto manifest=json::parse(text);std::string id=manifest.value("id",std::string{}),entry=manifest.value("entry",std::string{});
        if(!ValidPluginId(id)) throw std::runtime_error("无效的插件 ID");
        if(manifest.value("kind",std::string("soft"))!="soft" || (manifest.value("apiVersion",1u)<1 || manifest.value("apiVersion",1u)>CCRIDE_PLUGIN_API_VERSION)) throw std::runtime_error("不兼容的插件 API");
        auto dll=fs::u8path(entry);
        if(!ValidPluginEntry(entry) || !fs::is_regular_file(source/dll)) throw std::runtime_error("插件 DLL 无效");
        fs::path target=root/fs::u8path(id);
        if(fs::exists(target)) throw std::runtime_error("该插件已存在，请先在插件管理中卸载旧版");
        fs::rename(source,target);
        ScanAndLoad();RefreshInstalledFlags();
        g_app.console.Append("[插件] 已安装本地插件: "+id+"\n",IM_COL32(120,220,140,255));return true;
    } catch(const std::exception& e) {error=e.what();return false;}
}
