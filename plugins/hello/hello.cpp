// ccrIDE 硬编译插件示例: Hello (演示 API)
// 说明: kind=hard 的插件源码与 IDE 主程序一起编译链接 (见 build.ps1),
// 不再生成独立 DLL。导出函数名需唯一, 宿主通过 src/builtin_plugins.cpp 注册。
#include <imgui.h>
#include "../../src/plugin_api.h"
#include <cstdio>

static ImGuiContext* g_ctx = nullptr;
static PluginApi*    g_api = nullptr;
static bool          g_open = true;
static int           g_count = 0;

extern "C" __declspec(dllexport) void Hello_Plugin_OnLoad(void* ctx, PluginApi* api) {
    g_ctx = (ImGuiContext*)ctx;
    g_api = api;
    g_open = true;   // 宿主重新加载插件时再次显示窗口 (窗口被关闭后可恢复)
    ImGui::SetCurrentContext(g_ctx);
    if (g_api && g_api->Log) g_api->Log("[hello] 示例插件已加载 (硬编译)");
}

extern "C" __declspec(dllexport) void Hello_Plugin_OnFrame() {
    ImGui::SetCurrentContext(g_ctx);
    ImGui::SetNextWindowSize(ImVec2(300, 170), ImGuiCond_FirstUseEver);
    // NoFocusOnAppearing: 插件窗口不得抢走主编辑器的键盘焦点
    if (ImGui::Begin("示例插件 Hello", &g_open, ImGuiWindowFlags_NoFocusOnAppearing)) {
        ImGui::TextWrapped("这是插件 API 的最小示例: 硬编译插件随 ccrIDE.exe 一起构建。");
        ImGui::Separator();
        ImGui::Text("点击次数: %d", g_count);
        if (ImGui::Button("点我", ImVec2(100, 0))) {
            g_count++;
            if (g_api && g_api->Log) {
                char msg[64];
                snprintf(msg, sizeof(msg), "[hello] 点击了 %d 次", g_count);
                g_api->Log(msg);
            }
        }
        ImGui::Separator();
        ImGui::TextDisabled("在 插件 -> 插件管理 中可停用本插件");
    }
    ImGui::End();
}

extern "C" __declspec(dllexport) void Hello_Plugin_OnUnload() {
    if (g_api && g_api->Log) g_api->Log("[hello] 示例插件已卸载");
}
