#pragma once
// MiniCPP - 插件系统 + 插件市场
#include <string>
#include <vector>
#include <atomic>
#include <deque>
#include <mutex>
#include <thread>
#include <windows.h>
#include "plugin_api.h"

class CodeEditor;
struct PluginInfo {
    std::string  id, name, version, author, description, entry, loadError;
    std::string  kind = "soft";   // "hard"=随主程序编译内置, "soft"=外部 DLL 动态加载
    std::wstring dir;       // E:\cPP\plugins\<id>
    std::wstring dllPath;
    unsigned int apiVersion = 1;
    bool   bundled = false;
    bool   loaded  = false;
    bool   enabled = true;  // 用户是否启用 (settings.disabledPlugins 取反)
    HMODULE hmod = nullptr;
    Plugin_OnLoadFn   onLoad   = nullptr;
    Plugin_OnFrameFn  onFrame  = nullptr;
    Plugin_OnUnloadFn onUnload = nullptr;
};

struct MarketItem {          // 市场条目 (随附 + 远程)
    unsigned int taskCoordinatorVersion = 0;
    std::string id, name, version, author, description, entry, dllUrl, packageUrl;
    std::string sha256;      // DLL 摘要 (可选, 安装时校验完整性)
    unsigned int apiVersion = 1;
    bool bundled   = false;
    bool installed = false;  // 插件目录已存在
    bool enabled   = true;
};

// 市场网络任务完成消息 (后台线程 -> 主线程)
enum class NetKind { FetchOk, FetchErr, InstallOk, InstallFail };
struct NetMsg {
    NetKind kind;
    std::string text;                 // 错误信息 / 插件名
    std::vector<MarketItem> items;    // FetchOk: 解析出的新条目
};

class PluginManager {
public:
    std::vector<PluginInfo>  plugins;
    std::vector<MarketItem>  catalog;   // 随附 + 远程合并后的市场列表
    bool catalogLoaded = false;
    std::string lastMarketError;

    // 网络后台任务 (单线程; catalog 等共享数据只在主线程读写)
    std::thread m_worker;
    std::mutex  m_resMtx;
    std::deque<NetMsg> m_msgs;
    std::atomic<bool> m_netBusy{false};

    // 扫描 E:\cPP\plugins\*\plugin.json 并加载 DLL
    void ScanAndLoad();
    void UnloadAll();
    void ReloadAll();
    bool HasCompletionProvider() const;
    void BeginCompletion();
    std::string CompletionDocumentation(const std::string& text,size_t offset);
    void DrawCompletionDetails(CodeEditor& editor);
    void DrawCommands();
    void SetDockTargets(unsigned int left,unsigned int bottom,unsigned int right);
    bool InstallLocalArchive(const std::wstring& archive,std::string& error,const MarketItem* expected=nullptr,bool activate=true);
    void CallFrames();       // 每帧在主循环调用

    // 硬编译插件 (kind=hard) 入口由主程序编译时静态链接, 注册查找函数供扫描匹配
    PluginBuiltinLookupFn builtinLookup = nullptr;
    void SetBuiltinLookup(PluginBuiltinLookupFn fn) { builtinLookup = fn; }

    // 管理窗口: 列出所有插件并允许 启用/停用 (停用即不再加载)
    void DrawPluginsWindow(bool* open);
    void TogglePluginEnabled(const std::string& id);   // 翻转后立即重载
    bool showCompile = false;      // 编译软插件(源码->DLL) 弹窗开关
    std::string compileDir;        // 待编译插件目录 (含 plugin.json)

    // 市场
    void LoadBundledCatalog();                   // 内置示例
    void RequestFetchRemote();                   // 主线程调用: 后台拉取线上目录
    void RequestInstall(const MarketItem& it);   // 主线程调用: 后台下载安装
    void PumpNetResults();                       // 主线程每帧调用: 消费网络结果
    void Shutdown();                             // 退出前 join 网络线程
    bool Uninstall(const std::string& id, std::string& err);

    void RefreshInstalledFlags();
    void DrawMarketWindow(bool* open);
    void PushMsg(NetKind kind, std::string text, std::vector<MarketItem> items = {});

    int Count() const { return (int)plugins.size(); }
};
