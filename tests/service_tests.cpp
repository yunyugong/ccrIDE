#include "service_client.h"
#include <iostream>
#include <thread>
#define CHECK(x) do {if(!(x)){std::cerr<<"FAIL "<<__LINE__<<": "<<#x<<'\n';return 1;}++passed;}while(0)
int wmain(int argc,wchar_t** argv) {
    if(argc!=2)return 2;int passed=0;
    auto call=[&](const std::string& method,DWORD timeout=1500){
        std::atomic<bool> cancel{false};return service::Call(argv[1],method,{{"apiKey","test-secret"}},cancel,{},timeout);
    };
    std::atomic<bool> cancel{false};std::string text;
    CHECK(service::Call(argv[1],"healthy",{},cancel,[&](const auto& value){text+=value;}).ok);CHECK(text==u8"你好");
    for(auto method:{"crash","bad-id","bad-json","oversized","missing-done","flood","trailing","failed-after-done"}) {
        auto start=GetTickCount64();auto reply=call(method);
        CHECK(!reply.ok);CHECK(!reply.error.empty());CHECK(reply.error.find("test-secret")==std::string::npos);CHECK(GetTickCount64()-start<3000);
        CHECK(call("healthy").ok); // A failed service must not poison the next request.
    }
    auto start=GetTickCount64();CHECK(!call("hang",150).ok);CHECK(GetTickCount64()-start<1500);
    cancel=false;std::thread stop([&](){Sleep(120);cancel=true;});
    auto reply=service::Call(argv[1],"hang",{},cancel,{},10000);stop.join();CHECK(!reply.ok);CHECK(GetTickCount64()-start<2000);
    cancel=false;CHECK(!service::Call(argv[1],"healthy",{{"text",std::string(2*1024*1024,'x')}},cancel,{}).ok);
    CHECK(call("healthy").ok);
    std::cout<<"Service fault containment: "<<passed<<" passed\n";return 0;
}
