#pragma once
// MiniCPP - AI 助手 (OpenAI 兼容接口, 实验性)
#include <string>
#include <vector>
#include <atomic>
#include <mutex>
#include <thread>

struct AIMsg {
    std::string role;   // "user" / "assistant" / "system"
    std::string content;
};

struct AIState {
    std::atomic<bool> busy{false};
    std::atomic<bool> streaming{false};   // 是否当前是流式输出中
    std::atomic<bool> stopRequested{false};  // 用户提前停止
    std::mutex mtx;
    std::string result;     // 最近一次回复 (流式: 累积到这里)
    std::string error;
    std::string prompt;     // 输入框内容 (UI 线程持有)
    bool includeSelection = true;
    bool useStream = true;  // 默认使用流式 (用户可切换)
    std::thread worker;     // 单工作线程 (busy=false 即已结束; 退出时 join)

    // 多轮对话: 完整历史 (不含 system prompt, 发送时自动拼接)
    // 注: result 是 assistant 正在累加的临时副本; 发送结束后把 prompt + result 作为一条消息加入 history
    std::vector<AIMsg> history;
    static constexpr int MAX_HISTORY = 40;   // 最多保留 20 轮 (40 条 user/assistant)

    void Send(const std::string& userPrompt, const std::string& selectedCode);
    void Stop();            // 提前中止流式输出
    void ClearHistory();    // 清空对话历史
    void DrawWindow(bool* open);
    void TestConnection();
    void Shutdown();        // 退出前调用: 等待工作线程结束
};
