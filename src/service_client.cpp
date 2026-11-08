#include "i18n.h"
#include "service_client.h"
#include <filesystem>
#include <algorithm>

namespace service {
Reply Call(const std::wstring& executable,const std::string& method,const nlohmann::json& payload,
           std::atomic<bool>& cancel,const std::function<void(const std::string&)>& onText,DWORD timeoutMs) {
    static std::atomic<unsigned long long> nextId{0};
    constexpr size_t frameLimit=256*1024,requestLimit=2*1024*1024;
    Reply reply;std::string pending,protocolError;bool done=false;
    try {
        DWORD attributes=GetFileAttributesW(executable.c_str());
        if(attributes==INVALID_FILE_ATTRIBUTES || (attributes&FILE_ATTRIBUTE_DIRECTORY))
            return {false,i18n::Tr("找不到服务程序，请检查插件安装是否完整")};
        auto id=++nextId;
        execution::Request request;request.lane=execution::Lane::Service;request.label=i18n::Tr("AI 服务");request.logOutput=false;
        request.exe=executable;request.cwd=std::filesystem::path(executable).parent_path().wstring();
        request.input=nlohmann::json{{"v",1},{"id",id},{"method",method},{"payload",payload}}.dump()+"\n";
        if(request.input.size()>requestLimit) {reply.error=i18n::Tr("服务请求超过 2 MiB 限制，请缩短输入或清空历史");return reply;}
        request.capture=false;request.cancel=&cancel;
        request.limits.timeMs=std::clamp<DWORD>(timeoutMs,100,300000);request.limits.memoryBytes=256ull*1024*1024;request.limits.outputBytes=8*1024*1024;
        auto reject=[&](){protocolError=i18n::Tr("服务返回无效或超限消息");cancel=true;};
        request.onOutput=[&](const std::string& bytes,bool stderrChannel) {
            if(stderrChannel || !protocolError.empty())return;
            // Parse incrementally; no unbounded buffers even without a newline.
            for(char c:bytes) {
                if(c!='\n') {
                    if(pending.size()>=frameLimit) {reject();return;}
                    pending+=c;continue;
                }
                try {
                    auto event=nlohmann::json::parse(pending);pending.clear();
                    if(done || event.value("v",0)!=1 || !event.contains("id") || !event["id"].is_number_unsigned() || event["id"].get<unsigned long long>()!=id) {reject();return;}
                    auto type=event.value("type",std::string{});
                    if(type=="text") {
                        auto text=event.at("text").get<std::string>();if(onText)onText(text);
                    } else if(type=="done") {
                        done=true;reply.ok=event.at("ok").get<bool>();reply.error=event.value("error",std::string{});
                    } else {reject();return;}
                }catch(...) {reject();return;}
            }
        };
        auto result=execution::Run(request);
        if(!protocolError.empty()) {reply.ok=false;reply.error=protocolError;}
        else if(result.reason!=execution::Reason::Exited) {reply.ok=false;reply.error=std::string(i18n::Tr("服务已停止: "))+execution::ReasonText(result.reason);}
        else if(result.exitCode || !done || !pending.empty()) {reply.ok=false;reply.error=i18n::Tr("服务异常退出或未完成响应，可重试");}
        return reply;
    }catch(...) {return {false,i18n::Tr("无法启动或处理服务请求")};}
}
}
