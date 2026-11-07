#pragma once
// MiniCPP - 硬编译插件注册表 (kind=hard)
// 硬编译插件的源码与主程序一起编译链接 (见 build.ps1), 这里按插件 id
// 提供其入口函数指针, 供 PluginManager::ScanAndLoad 静态匹配, 无需 LoadLibrary。
#include "plugin_api.h"

// 按插件 id 查找; 未内置返回 nullptr
const PluginFns* BuiltinPluginLookup(const char* id);
