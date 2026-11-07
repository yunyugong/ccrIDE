#include "ai_assist.h"
#include "../../src/plugin_api.h"
#include <filesystem>
#include <fstream>
#include "utils.h"
#include "service_client.h"
#include "ui_helpers.h"
#include <imgui.h>
#include <nlohmann/json.hpp>
#include <thread>
#include <algorithm>

using nlohmann::json;
static PluginApi* g_api=nullptr;
static bool g_open=true,g_settingsOpen=false;
struct AISettings { bool enabled=false; std::string baseUrl="http://127.0.0.1:11434/v1",apiKey,model="qwen2.5-coder:7b"; };
static AISettings settings;
static std::filesystem::path settingsPath;
static std::string Selection() {
    std::string text; g_api->GetSelection([](void* out,const char* value,size_t size){ static_cast<std::string*>(out)->assign(value,size); },&text); return text;
}
static void SaveSettings() {
    std::filesystem::create_directories(settingsPath.parent_path());
    json j={{"enabled",settings.enabled},{"baseUrl",settings.baseUrl},{"apiKey",settings.apiKey},{"model",settings.model}};
    std::ofstream(settingsPath,std::ios::binary)<<j.dump(2);
}
static void Open() { g_open=true; }
static void OpenSettings() {g_settingsOpen=true;}
static void DrawSettings() {
    ImGui::SetNextWindowSize(ImVec2(460,340),ImGuiCond_FirstUseEver);
    if(ImGui::Begin("AI 助手设置",&g_settingsOpen)) {
        ImGui::Checkbox("启用 AI",&settings.enabled);
        ImGui::TextUnformatted("接口地址");ImGui::SetNextItemWidth(-FLT_MIN);ui::InputTextStd("##baseUrl",settings.baseUrl);
        ImGui::TextUnformatted("模型");ImGui::SetNextItemWidth(-FLT_MIN);ui::InputTextStd("##model",settings.model);
        ImGui::TextUnformatted("API Key");ImGui::SetNextItemWidth(-FLT_MIN);ui::InputTextStd("##apiKey",settings.apiKey,ImGuiInputTextFlags_Password);
        ImGui::Spacing();
        if(ImGui::Button("保存插件设置")) {SaveSettings();g_settingsOpen=false;}
        ImGui::TextWrapped("设置独立保存在 AI 插件的数据目录中。发送前可选择是否附带当前选中代码。");
    }
    ImGui::End();
}
// Keep actions accessible in narrow docked panels.
static void Flow(const char* nextLabel,float width=0) {
    float next=width>0 ? width : ImGui::CalcTextSize(nextLabel).x+ImGui::GetStyle().FramePadding.x*2;
    float edge=ImGui::GetWindowPos().x+ImGui::GetWindowContentRegionMax().x;
    if(ImGui::GetItemRectMax().x+ImGui::GetStyle().ItemSpacing.x+next<=edge) ImGui::SameLine();
}


static std::wstring ServiceExecutable() {
    static int moduleMarker;HMODULE module=nullptr;wchar_t path[32768]{};
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(&moduleMarker),&module))return {};
    DWORD n=GetModuleFileNameW(module,path,32768);if(!n || n==32768)return {};
    return (std::filesystem::path(path).parent_path()/L"ai-service.exe").wstring();
}

void AIState::Send(const std::string& userPrompt, const std::string& selectedCode) {
    if (busy.load()) return;
    {
        std::lock_guard<std::mutex> lk(mtx);
        result.clear();
        error.clear();
    }
    stopRequested.store(false);
    busy.store(true);

    // Snapshot plugin-owned settings before starting the worker.
    std::string base  = settings.baseUrl;
    std::string key   = settings.apiKey;
    std::string model = settings.model;

    // busy 为 false 说明上一个工作线程已结束, 先回收再启动
    if (worker.joinable()) worker.join();
    bool requestStream=useStream;
    worker = std::thread([this, userPrompt, selectedCode, base, key, model,requestStream]() {
        if (base.empty() || model.empty()) {
            std::lock_guard<std::mutex> lk(mtx);
            error = "请先在 AI 助手的连接设置中配置接口地址和模型";
            busy.store(false);
            streaming.store(false);
            return;
        }
        std::string content = userPrompt;
        if (!selectedCode.empty()) {
            content += "\n\n选中代码:\n```cpp\n" + selectedCode + "\n```";
        }

        // 构造消息: system + history + 本轮 user
        json msgs = json::array();
        msgs.push_back(json{{"role", "system"}, {"content", "你是 ccrIDE 的 AI 助手插件，一个 C/C++ 编程助手, 回答简洁准确, 代码用 markdown 代码块给出."}});
        {
            std::lock_guard<std::mutex> lk(mtx);
            for (auto& m : history) {
                msgs.push_back(json{{"role", m.role}, {"content", m.content}});
            }
        }
        msgs.push_back(json{{"role", "user"}, {"content", content}});

        bool doStream = requestStream;
        json body = {
            {"model", model},
            {"stream", doStream},
            {"temperature", 0.2},
            {"messages", msgs}
        };

        // 最终实际发送给 API 的 user content (带选中代码), 用于写入 history
        std::string finalUserContent = content;

        streaming.store(doStream);
        auto reply=service::Call(ServiceExecutable(),"chat",json{{"baseUrl",base},{"apiKey",key},{"body",body}},stopRequested,[this](const std::string& text){
            std::lock_guard<std::mutex> lock(mtx);result+=text;
        });
        std::string finalResult;
        {
            std::lock_guard<std::mutex> lock(mtx);
            if(!reply.ok)error=reply.error;
            else finalResult=result;
        }

        // 成功且无错误 -> 把本轮 user + assistant 写入 history
        bool hasError;
        {
            std::lock_guard<std::mutex> lk(mtx);
            hasError = !error.empty();
        }
        if (!hasError && !finalResult.empty()) {
            std::lock_guard<std::mutex> lk(mtx);
            history.push_back({"user", finalUserContent});
            history.push_back({"assistant", finalResult});
            // 限制长度, 超出从头截断 (保留 system 在 API 里拼接, history 里不用)
            while ((int)history.size() > MAX_HISTORY) {
                history.erase(history.begin(), history.begin() + 2);
            }
        }
        busy.store(false);
        streaming.store(false);
    });
}

void AIState::Stop() {
    stopRequested.store(true);
}

void AIState::ClearHistory() {
    if (busy.load()) return;
    std::lock_guard<std::mutex> lk(mtx);
    history.clear();
    result.clear();
    error.clear();
}

void AIState::Shutdown() {
    stopRequested.store(true);
    if (worker.joinable()) worker.join();
}

void AIState::TestConnection() {
    if (busy.exchange(true)) return;
    std::string base=settings.baseUrl,key=settings.apiKey;
    if(worker.joinable())worker.join();
    stopRequested=false;
    {std::lock_guard<std::mutex> lock(mtx);result.clear();error.clear();}
    worker=std::thread([this,base,key](){
        auto reply=service::Call(ServiceExecutable(),"models",json{{"baseUrl",base},{"apiKey",key}},stopRequested,[this](const std::string& text){
            std::lock_guard<std::mutex> lock(mtx);result+=text;
        },30000);
        if(!reply.ok){std::lock_guard<std::mutex> lock(mtx);error=reply.error;}
        busy=false;
    });
}

void AIState::DrawWindow(bool* open) {
    g_api->DockNextWindow("AI 助手",2);
    if (!ImGui::Begin("AI 助手", open)) { ImGui::End(); return; }

    if(ImGui::Button("连接设置…")) g_settingsOpen=true;
    Flow("未启用");ImGui::TextDisabled("%s",settings.enabled ? "已启用" : "未启用");
    ImGui::Checkbox("附带编辑器选中代码", &includeSelection);
    Flow("流式输出",105);
    ImGui::Checkbox("流式输出", &useStream);
    Flow("测试连接");
    if (ImGui::Button("测试连接")) TestConnection();
    Flow("清空结果");
    if (ImGui::Button("清空结果")) { std::lock_guard<std::mutex> lk(mtx); result.clear(); error.clear(); }
    Flow("清空对话");
    {
        std::lock_guard<std::mutex> lk(mtx);
        ImGui::BeginDisabled(busy.load() || history.empty());
    }
    if (ImGui::Button("清空对话")) ClearHistory();
    ImGui::EndDisabled();
    Flow("40 条历史");
    {
        std::lock_guard<std::mutex> lk(mtx);
        ImGui::TextDisabled("%zu 条历史", history.size());
    }

    ImGui::Separator();

    // 历史对话区 (只读, 折叠显示)
    bool isStream = streaming.load();
    bool sending = busy.load();
    ImVec2 avail = ImGui::GetContentRegionAvail();
    // 历史区 + 提示区 + 按钮区 + 状态提示
    float historyH = std::max(120.0f, avail.y - 230.0f);
    if (ImGui::BeginChild("##ai_history", ImVec2(0, historyH), true, ImGuiWindowFlags_HorizontalScrollbar)) {
        std::string lastAssist;
        {
            std::lock_guard<std::mutex> lk(mtx);
            if (!history.empty())
                for (auto it = history.rbegin(); it != history.rend(); ++it)
                    if (it->role == "assistant") { lastAssist = it->content; break; }
        }
        std::lock_guard<std::mutex> lk(mtx);
        bool hasHistoryOrResult = !history.empty() || !result.empty() || !error.empty();
        if (!hasHistoryOrResult) {
            ImGui::TextDisabled("(暂无对话历史)");
        } else {
            // 历史
            for (size_t i = 0; i < history.size(); i++) {
                auto& m = history[i];
                if (m.role == "user") {
                    ImGui::TextColored(ImVec4(0.6f, 0.85f, 1.0f, 1), "你:");
                    ImGui::SameLine();
                    ImGui::TextWrapped("%s", m.content.c_str());
                } else {
                    ImGui::TextColored(ImVec4(0.55f, 1.0f, 0.75f, 1), "AI:");
                    ImGui::SameLine();
                    ImGui::TextWrapped("%s", m.content.c_str());
                }
                ImGui::Separator();
            }
            // 本轮 user (发送中) + 正在生成的 result
            if (sending) {
                ImGui::TextColored(ImVec4(0.6f, 0.85f, 1.0f, 1), "你:");
                ImGui::SameLine();
                std::string curPrompt = prompt;
                if (includeSelection) {
                    if (!Selection().empty())
                        curPrompt += "  [附选中代码]";
                }
                ImGui::TextWrapped("%s", curPrompt.c_str());
                ImGui::Separator();
                if (!error.empty()) {
                    ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "AI 错误: %s", error.c_str());
                } else {
                    ImGui::TextColored(ImVec4(0.55f, 1.0f, 0.75f, 1), "AI:");
                    ImGui::SameLine();
                    ImGui::TextWrapped("%s", result.c_str());
                    if (isStream) ImGui::SameLine(), ImGui::TextColored(ImVec4(1,1,0.5f,1), "▊");
                }
            } else {
                // 空闲时也常驻显示最近错误/测试连接结果, 否则失败会"静默消失", 用户无从排查
                if (!error.empty())
                    ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "AI 错误: %s", error.c_str());
                else if (!result.empty() && result != lastAssist) {
                    // 测试连接等未进 history 的结果; 若 result 只是上轮回复的副本则不重复显示
                    if (history.empty()) {
                        ImGui::TextColored(ImVec4(0.55f, 1.0f, 0.75f, 1), "AI:");
                        ImGui::SameLine();
                        ImGui::TextWrapped("%s", result.c_str());
                    }
                }
            }
        }
        if (isStream) ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();

    ImGui::Text("提示:");
    ImGui::PushFont(ImGui::GetFont());
    ui::InputTextMultilineStd("##ai_prompt", prompt, ImVec2(-1, 90),
                              ImGuiInputTextFlags_AllowTabInput |
                              (sending ? ImGuiInputTextFlags_ReadOnly : 0));
    ImGui::PopFont();

    ImGui::BeginDisabled(sending || !settings.enabled);
    if (ImGui::Button("发送", ImVec2(120, 0)) && !prompt.empty()) {
        std::string sel;
        if (includeSelection) {
            sel = Selection();
        }
        Send(prompt, sel);
    }
    Flow("生成代码",120);
    if (ImGui::Button("生成代码", ImVec2(120, 0)) && !prompt.empty()) {
        std::string p = "请用 C++ 编写: " + prompt + "\n只输出代码, 不要额外解释.";
        Send(p, "");
    }
    ImGui::EndDisabled();
    Flow("停止");
    if (isStream) {
        if (ImGui::Button("停止")) Stop();
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.7f, 1), "生成中...");
    } else if (sending) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1, 1, 0.5f, 1), "请求中...");
    }

    std::string resultCopy;
    {
        std::lock_guard<std::mutex> lk(mtx);
        // 如果有正在生成的 result, 优先用它; 否则用 history 最后一条 assistant
        if (!result.empty()) {
            resultCopy = result;
        } else if (!history.empty()) {
            for (auto it = history.rbegin(); it != history.rend(); ++it) {
                if (it->role == "assistant") { resultCopy = it->content; break; }
            }
        }
    }
    Flow("复制结果");
    if (ImGui::Button("复制结果") && !resultCopy.empty())
        ImGui::SetClipboardText(resultCopy.c_str());
    Flow("插入到编辑器");
    if (ImGui::Button("插入到编辑器") && !resultCopy.empty()) {
        {
            std::string code = resultCopy;
            size_t p1 = code.find("```cpp");
            if (p1 != std::string::npos) {
                size_t p2 = code.find("```", p1 + 5);
                if (p2 != std::string::npos) {
                    code = code.substr(p1 + 5, p2 - (p1 + 5));
                    while (!code.empty() && (code.front() == '\n' || code.front() == '\r')) code.erase(code.begin());
                    while (!code.empty() && (code.back() == '\n' || code.back() == '\r')) code.pop_back();
                }
            }
            g_api->InsertText(code.c_str());
        }
    }

    ImGui::End();
}

static AIState state;
extern "C" __declspec(dllexport) void Plugin_OnLoadV2(void* context,PluginApi* api) {
    g_api=api; g_open=true; ImGui::SetCurrentContext((ImGuiContext*)context);
    ImGui::SetAllocatorFunctions(api->Allocate,api->Deallocate,api->allocatorUser);
    settingsPath=std::filesystem::u8path(api->GetDataDir())/"plugins"/"ai-assistant"/"settings.json";
    try {
        json j=json::object();
        if(std::filesystem::exists(settingsPath)) std::ifstream(settingsPath)>>j;
        else { std::ifstream legacy(std::filesystem::u8path(api->GetDataDir())/"settings.json"); if(legacy) { json old;legacy>>old;j=old.value("ai",json::object()); } }
        settings.enabled=j.value("enabled",false); settings.baseUrl=j.value("baseUrl",settings.baseUrl);
        settings.model=j.value("model",settings.model); settings.apiKey=j.value("apiKey",std::string{});
        if(!std::filesystem::exists(settingsPath)) SaveSettings();
    } catch(...) { api->Log("[AI 插件] 无法读取设置，已使用默认值"); }
    api->RegisterCommand("ai-assistant.open","AI 助手",Open);
    api->RegisterCommand("ai-assistant.settings","AI 助手设置",OpenSettings);
}
extern "C" __declspec(dllexport) void Plugin_OnFrame() { if(g_open) state.DrawWindow(&g_open);if(g_settingsOpen)DrawSettings(); }
extern "C" __declspec(dllexport) void Plugin_OnUnload() { state.Shutdown(); g_api=nullptr; }
