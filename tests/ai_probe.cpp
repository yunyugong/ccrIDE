// MiniCPP - AI 发送链路探针 (无 GUI)
// 用本地 mock 服务验证 AIState::Send: 请求是否发出 + 流式结果是否回写
#include <cstdio>
#include <chrono>
#include <thread>
#include "app.h"
#include "ai_assist.h"

int main() {
    g_app.settings.ai.baseUrl = "http://127.0.0.1:18899/v1";
    g_app.settings.ai.model   = "mock-model";
    g_app.settings.ai.enabled = true;

    AIState ai;
    ai.includeSelection = false;
    ai.useStream = true;
    std::printf("send start\n");
    ai.Send("你好, 请回复", "");

    // 轮询等待 worker 结束 (最多 8s)
    for (int i = 0; i < 80 && ai.busy.load(); i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (i % 10 == 0)
            std::printf("...busy streaming=%d\n", (int)ai.streaming.load());
    }
    std::printf("busy=%d streaming=%d\n", (int)ai.busy.load(), (int)ai.streaming.load());
    {
        std::lock_guard<std::mutex> lk(ai.mtx);
        std::printf("error=[%s]\n", ai.error.c_str());
        std::printf("result=[%s]\n", ai.result.c_str());
        std::printf("history=%zu\n", ai.history.size());
        if (!ai.history.empty()) {
            auto& last = ai.history.back();
            std::printf("last-assistant=[%s]\n", last.content.c_str());
        }
    }
    ai.Shutdown();
    return 0;
}
