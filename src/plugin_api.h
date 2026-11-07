#pragma once
// ccrIDE 插件 API - 宿主与插件共享的头文件
//
// 插件必须:
//   1. 与宿主使用相同版本的 ImGui 头文件编译, 并链接自己的 imgui 副本
//   2. 导出以下 C 函数 (示例):
//        extern "C" __declspec(dllexport) void Plugin_OnLoad(ImGuiContext* ctx, PluginApi* api);
//        extern "C" __declspec(dllexport) void Plugin_OnFrame(void);
//        extern "C" __declspec(dllexport) void Plugin_OnUnload(void);
//   3. 在 Plugin_OnLoad 中调用 ImGui::SetCurrentContext(ctx)
//
// 构建插件示例:
//   g++ -shared -O2 -std=c++17 -static-libgcc -static-libstdc++ \
//       -I<E:\cPP\imgui> plugin.cpp <imgui 核心源码> -o plugin.dll

#include <stddef.h>
#define CCRIDE_PLUGIN_API_VERSION 3

typedef void (*PluginTextSink)(void* user, const char* text, size_t length);
typedef struct PluginCompletionSource { const char* file; int line; const char* documentation; } PluginCompletionSource;
typedef struct PluginCompletionView {
    const char* label; const char* hint;
    const PluginCompletionSource* sources; size_t sourceCount;
    int* selectedSource;
} PluginCompletionView;
typedef struct PluginCompletionHooks {
    void (*Begin)(void);
    void (*Describe)(const char* text,size_t length,size_t offset,PluginTextSink sink,void* user);
    void (*Draw)(const PluginCompletionView* view);
} PluginCompletionHooks;

// V1's first three fields remain binary-compatible. V2 plugins export
// Plugin_OnLoadV2 so a V1 host cannot accidentally pass a shorter API struct.
// All strings/views are borrowed for the call; text sinks allocate on the host.
// 宿主提供给插件的服务
typedef struct PluginApi {
    const char* (*GetDataDir)(void);        // 数据目录 (D:\ccrIDE\ 之类)
    void        (*Log)(const char* msg);    // 写入宿主日志/控制台
    const char* (*GetSetting)(const char* key); // 读取宿主设置 (gppPath/theme/marketUrl)
    unsigned int apiVersion;
    size_t structSize;
    void (*RegisterCommand)(const char* id,const char* label,void (*execute)(void));
    void (*RegisterCompletion)(const PluginCompletionHooks* hooks);
    void (*GetSelection)(PluginTextSink sink,void* user);
    void (*InsertText)(const char* text);
    void (*DockNextWindow)(const char* title,int location); // 0 left, 1 bottom, 2 right; FirstUseEver only
    void* (*Allocate)(size_t,void*);
    void (*Deallocate)(void*,void*);
    void* allocatorUser;
} PluginApi;

// The V2 layout above is frozen. V3 is an append-only extension; callbacks run
// only on the host UI thread. Plugins marshal worker results in OnFrame.
typedef struct PluginLanguageHooks {
    void (*Document)(const char* file,const char* text,size_t length,unsigned long long revision);
    void (*Close)(const char* file);
    void (*Request)(const char* kind,const char* file,unsigned long long revision,size_t byteOffset);
} PluginLanguageHooks;
typedef struct PluginApiV3 {
    PluginApi base;
    void (*RegisterLanguage)(const PluginLanguageHooks* hooks);
    void (*PublishLanguage)(const char* json,size_t length);
} PluginApiV3;

// 插件元信息 (从 plugin.json 读取)
// {
//   "id": "clock",
//   "name": "时钟",
//   "version": "1.0",
//   "author": "ccrIDE",
//   "description": "...",
//   "entry": "clock.dll"
// }

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*Plugin_OnLoadFn)(void* imguiCtx, PluginApi* api);
typedef void (*Plugin_OnFrameFn)(void);
typedef void (*Plugin_OnUnloadFn)(void);

#ifdef __cplusplus
}
#endif

// 硬编译插件 (kind=hard) 宿主侧注册描述:
// 源码随主程序一起编译, 宿主通过本结构体直接持有其入口函数指针, 无需 LoadLibrary。
typedef struct PluginFns {
    Plugin_OnLoadFn   onLoad;
    Plugin_OnFrameFn  onFrame;
    Plugin_OnUnloadFn onUnload;
} PluginFns;
typedef const PluginFns* (*PluginBuiltinLookupFn)(const char* id);   // 按插件 id 查找硬编译入口

#define PLUGIN_LOAD_NAME   "Plugin_OnLoad"
#define PLUGIN_FRAME_NAME  "Plugin_OnFrame"
#define PLUGIN_UNLOAD_NAME "Plugin_OnUnload"
