#include <windows.h>
#include <nlohmann/json.hpp>
#include <iostream>
using nlohmann::json;
int main() {
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    std::string input;std::getline(std::cin,input);auto request=json::parse(input);
    auto id=request.at("id").get<unsigned long long>();auto method=request.at("method").get<std::string>();
    auto emit=[&](json value){value["v"]=1;value["id"]=id;std::cout<<value.dump()<<'\n'<<std::flush;};
    if(method=="crash")TerminateProcess(GetCurrentProcess(),42);
    if(method=="hang")Sleep(INFINITE);
    if(method=="bad-id") {std::cout<<json{{"v",1},{"id",id+1},{"type","done"},{"ok",true}}.dump()<<'\n'<<std::flush;Sleep(INFINITE);}
    if(method=="bad-json") {std::cout<<"not-json test-secret\n"<<std::flush;Sleep(INFINITE);}
    if(method=="oversized") {std::cout<<std::string(300000,'x')<<std::flush;Sleep(INFINITE);}
    if(method=="missing-done")return 0;
    if(method=="flood")for(int i=0;i<2000;++i)emit({{"type","text"},{"text",std::string(8192,'x')}});
    emit({{"type","text"},{"text",u8"你好"}});
    emit({{"type","done"},{"ok",true},{"error",""}});
    if(method=="trailing")std::cout<<"trailing"<<std::flush;
    if(method=="failed-after-done")return 5;
    return 0;
}
