#include "i18n.h"
// ccrIDE 硬编译插件示例: 时钟
// 说明: kind=hard 的插件源码与 IDE 主程序一起编译链接 (见 build.ps1),
// 不再生成独立 DLL。导出函数名需唯一, 宿主通过 src/builtin_plugins.cpp 注册。
#include <imgui.h>
#include "../../src/plugin_api.h"
#include <ctime>
#include <cstdio>

static ImGuiContext* g_ctx = nullptr;
static PluginApi*    g_api = nullptr;
static bool          g_open = true;

extern "C" __declspec(dllexport) void Clock_Plugin_OnLoad(void* ctx, PluginApi* api) {
    g_ctx = (ImGuiContext*)ctx;
    g_api = api;
    g_open = true;   // 宿主重新加载插件时再次显示窗口 (窗口被关闭后可恢复)
    ImGui::SetCurrentContext(g_ctx);
    if (g_api && g_api->Log) g_api->Log(i18n::Tr("[clock] 时钟插件已加载 (硬编译)"));
}

extern "C" __declspec(dllexport) void Clock_Plugin_OnFrame() {
    ImGui::SetCurrentContext(g_ctx);
    ImGui::SetNextWindowSize(ImVec2(300, 150), ImGuiCond_FirstUseEver);
    // NoFocusOnAppearing: 插件窗口不得抢走主编辑器的键盘焦点
    if (ImGui::Begin(i18n::Window("时钟"), &g_open, ImGuiWindowFlags_NoFocusOnAppearing)) {
        time_t t = time(nullptr);
        struct tm tmv;
        localtime_s(&tmv, &t);
        char buf[64];
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmv);
        ImGui::TextUnformatted(buf);
        ImGui::Separator();
        ImGui::TextUnformatted(i18n::Tr("硬编译示例插件 (随 ccrIDE.exe 构建)"));
        if (g_api && g_api->GetDataDir) {
            ImGui::TextDisabled(i18n::Tr("数据目录: %s"), g_api->GetDataDir());
        }
        ImGui::TextDisabled(i18n::Tr("设置主题: %s"), g_api && g_api->GetSetting ? g_api->GetSetting("theme") : "?");
        ImGui::Separator();
        ImGui::TextDisabled(i18n::Tr("在 插件 -> 插件管理 中可停用本插件"));
    }
    ImGui::End();
}

extern "C" __declspec(dllexport) void Clock_Plugin_OnUnload() {
    if (g_api && g_api->Log) g_api->Log(i18n::Tr("[clock] 时钟插件已卸载"));
}
