#pragma once
#include "app.h"
#include <filesystem>
static std::wstring testSavedPluginDir,testSavedDataDir;
static std::filesystem::path testPluginRoot;
static void LoadDocumentationTestPlugin() {
    namespace fs=std::filesystem;
    testSavedPluginDir=g_app.pluginDir;testSavedDataDir=g_app.dataDir;
    testPluginRoot=fs::temp_directory_path()/(L"ccride-plugin-test-"+std::to_wstring(GetCurrentProcessId()));
    fs::create_directories(testPluginRoot);
    fs::path source=fs::path(CCRIDE_TEST_PLUGIN_ROOT)/"completion-docs";
    fs::create_directories(testPluginRoot/"completion-docs");
    for(const char* name:{"plugin.json","completion-docs.dll"}) fs::copy_file(source/name,testPluginRoot/"completion-docs"/name,fs::copy_options::overwrite_existing);
    g_app.pluginDir=testPluginRoot.wstring();g_app.dataDir=(testPluginRoot/"data").wstring();
    fs::create_directories(g_app.dataDir);
    g_app.plugins.ScanAndLoad();
    if(!g_app.plugins.HasCompletionProvider()) throw std::runtime_error("Documentation DLL did not load");
}
static void UnloadDocumentationTestPlugin() {
    g_app.plugins.UnloadAll();g_app.pluginDir=testSavedPluginDir;g_app.dataDir=testSavedDataDir;
    if(testPluginRoot.parent_path()==std::filesystem::temp_directory_path()) {std::error_code ec;std::filesystem::remove_all(testPluginRoot,ec);}
}
