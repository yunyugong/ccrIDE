// Trusted AI network worker. Only framed UTF-8 JSON travels through inherited pipes.
// No keys/prompts in command line, environment overrides, temporary files or logs.
#include "utils.h"
#include <nlohmann/json.hpp>
#include <iostream>
#include <cstdio>
#include <fcntl.h>
#include <io.h>
using nlohmann::json;
static unsigned long long requestId=0;
static void Emit(json event) {
    event["v"]=1;event["id"]=requestId;
    std::cout<<event.dump()<<'\n'<<std::flush;
}
static void Text(const std::string& text) {
    for(size_t begin=0;begin<text.size();) {
        size_t end=std::min(text.size(),begin+16384);
        while(end<text.size() && (static_cast<unsigned char>(text[end])&0xc0)==0x80)--end;
        Emit({{"type","text"},{"text",text.substr(begin,end-begin)}});begin=end;
    }
}
static bool Stream(void*,const char* text,size_t length,bool) {if(length)Text(std::string(text,length));return bool(std::cout);}
int main() {
    _setmode(_fileno(stdin),_O_BINARY);_setmode(_fileno(stdout),_O_BINARY);
    try {
        std::string input;char buffer[4096];size_t n;
        while((n=fread(buffer,1,sizeof(buffer),stdin))!=0) {if(input.size()+n>2*1024*1024)return 2;input.append(buffer,n);}
        auto request=json::parse(input);
        if(request.value("v",0)!=1 || !request.at("id").is_number_unsigned())return 2;
        requestId=request["id"].get<unsigned long long>();
        auto method=request.at("method").get<std::string>();auto payload=request.at("payload");
        std::string base=payload.at("baseUrl").get<std::string>(),key=payload.value("apiKey",std::string{});
        while(!base.empty() && base.back()=='/')base.pop_back();
        if(base.empty())throw std::runtime_error("请配置接口地址");
        std::string response,error;bool ok=false;
        if(method=="models") {
            ok=utils::HttpGet(base+"/models",response,error,key);
            if(ok) {auto body=json::parse(response);std::string names="连接成功, 可用模型: ";for(auto& model:body.at("data"))names+=model.value("id",std::string{})+"  ";Text(names);}
        } else if(method=="chat") {
            auto body=payload.at("body");
            if(body.value("stream",false))ok=utils::HttpPostJsonStream(base+"/chat/completions",body.dump(),key,error,Stream,nullptr);
            else {
                ok=utils::HttpPostJson(base+"/chat/completions",body.dump(),key,response,error);
                if(ok) {
                    auto answer=json::parse(response);
                    if(answer.contains("error")) {ok=false;error=answer["error"].value("message",std::string("接口返回错误"));}
                    else {auto text=answer.at("choices").at(0).at("message").at("content").get<std::string>();Text(text.empty()?"(空回复)":text);}
                }
            }
        } else throw std::runtime_error("不支持的服务方法");
        Emit({{"type","done"},{"ok",ok},{"error",error.substr(0,32768)}});return 0;
    }catch(...) {
        try {Emit({{"type","done"},{"ok",false},{"error","AI 服务请求或响应异常，请检查接口设置后重试"}});}catch(...){}
        return 0;
    }
}
