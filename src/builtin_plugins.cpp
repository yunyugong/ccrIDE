#include "builtin_plugins.h"
#include <cstring>

// 硬编译插件入口声明 (源码位于 plugins\<id>\*.cpp, 随主程序一起编译链接)
extern "C" {
    void Clock_Plugin_OnLoad(void*, PluginApi*);
    void Clock_Plugin_OnFrame(void);
    void Clock_Plugin_OnUnload(void);
    void Hello_Plugin_OnLoad(void*, PluginApi*);
    void Hello_Plugin_OnFrame(void);
    void Hello_Plugin_OnUnload(void);
}

static const PluginFns kClock = { Clock_Plugin_OnLoad, Clock_Plugin_OnFrame, Clock_Plugin_OnUnload };
static const PluginFns kHello = { Hello_Plugin_OnLoad, Hello_Plugin_OnFrame, Hello_Plugin_OnUnload };

const PluginFns* BuiltinPluginLookup(const char* id) {
    if (id && std::strcmp(id, "clock") == 0) return &kClock;
    if (id && std::strcmp(id, "hello") == 0) return &kHello;
    return nullptr;
}
