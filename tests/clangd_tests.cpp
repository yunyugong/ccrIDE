#include "../plugins/clangd/session.h"
#include "lsp_support.h"
#include "utils.h"
#include <iostream>
#include <filesystem>
#include <fstream>
#include <tlhelp32.h>
using clangd_plugin::json;
static int passed=0;
#define CHECK(x) do{if(!(x)){std::cerr<<"FAIL "<<__LINE__<<": "<<#x<<'\n';return 1;}++passed;}while(0)
int wmain(int argc,wchar_t** argv) {
    auto original=json{{"jsonrpc","2.0"},{"id",1},{"result",u8"中文😀"}};auto frame=lsp::Frame(original);lsp::Framer parser;int count=0;
    for(char c:frame)parser.Feed(std::string(1,c),[&](const auto& message){if(message==original)++count;});CHECK(count==1);
    parser.Feed(frame+frame,[&](const auto&){++count;});CHECK(count==3);
    for(auto bad:{"Content-Length: -1\r\n\r\n","Content-Length: 90000000\r\n\r\n","Content-Length: 1\r\nContent-Length: 1\r\n\r\n0"}){bool rejected=false;try{lsp::Framer f;f.Feed(bad,[](const auto&){});}catch(...){rejected=true;}CHECK(rejected);}
    std::string unicode=u8"中文😀x\nint value;";
    CHECK(lsp::Position(unicode,10).at("character")==4);CHECK(lsp::Offset(unicode,{{"line",0},{"character",4}})==10);
    bool rejected=false;try{lsp::Offset(unicode,{{"line",0},{"character",3}});}catch(...){rejected=true;}CHECK(rejected);
    auto path=std::string(u8"C:/文件 空格/a#b.cpp");CHECK(lsp::Path(lsp::Uri(path))==path);
    if(argc<2 || GetFileAttributesW(argv[1])==INVALID_FILE_ATTRIBUTES){std::cout<<"LSP protocol: "<<passed<<" passed; real clangd not installed\n";return 77;}
    namespace fs=std::filesystem;auto root=fs::temp_directory_path()/(L"ccride-clangd-中文-"+std::to_wstring(GetCurrentProcessId()));fs::create_directories(root);
    auto file=root/L"case.cpp";auto key=lsp::FileKey(utils::WToU8(file.wstring()));clangd_plugin::Session session;
    clangd_plugin::Config config;config.executable=argv[1];config.compiler=argc>=3?argv[2]:L"";config.flags={"-std=c++17"};

    auto wait=[&](auto predicate,int timeout=15000){auto start=GetTickCount64();while(GetTickCount64()-start<(unsigned)timeout){for(auto& event:session.Poll()){if(predicate(event))return true;if(event.value("kind","")=="status")std::cout<<event.value("message","")<<'\n';}Sleep(15);}return false;};
    session.Start(config);CHECK(wait([](const auto& e){return e.value("kind","")=="status"&&e.value("ready",false);}));
    std::string text="template<class T> struct Box { T value; void clear(); };\nusing Alias=Box<int>;\nint main(){Alias box; box. }\n";
    utils::WriteFileU8(file.wstring(),text);size_t offset=text.find("box.")+4;session.Document(key,text,1);session.Request("completion",key,1,offset);
    CHECK(wait([](const auto& e){if(e.value("kind","")!="completion"||e.value("failed",false))return false;for(auto& item:e.at("items"))if(item.at("text")=="value")return true;return false;}));
    text="#include <vector>\nusing Nested=std::vector<std::vector<int>>;\nint main(){Nested data; data. }\n";offset=text.find("data.")+5;session.Document(key,text,2);session.Request("completion",key,2,offset);
    CHECK(wait([](const auto& e){if(e.value("kind","")!="completion"||e.value("failed",false))return false;for(auto& item:e.at("items"))if(item.at("text").template get<std::string>().find("push_back")!=std::string::npos)return true;return false;}));
    text="int broken = \"text\";\n";session.Document(key,text,3);
    CHECK(wait([](const auto& e){return e.value("kind","")=="diagnostics"&&e.value("revision",0)==3&&!e.at("items").empty();}));
    text="struct Item { int value; };\nint main(){Item x; return x.value;}\n";offset=text.rfind("value")+2;session.Document(key,text,4);session.Request("definition",key,4,offset);
    CHECK(wait([&](const auto& e){return e.value("kind","")=="definition"&&!e.value("failed",false)&&e.value("target","")==key&&e.at("position").at("line")==0;}));
    text="int value;\nint f(){int value=3;return value;}\nint g(){return value;}\n";session.Document(key,text,7);session.Request("rename:renamed",key,7,5);
    CHECK(wait([](const auto& e){return e.value("kind","")=="rename"&&!e.value("failed",false)&&e.at("edits").size()==2&&e.at("edits")[0].at("text")=="renamed";}));
    // A real unexpected service exit must be reported, and a fresh process can reconnect.
    bool killed=false;HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);PROCESSENTRY32W process{};process.dwSize=sizeof(process);
    if(snapshot!=INVALID_HANDLE_VALUE){if(Process32FirstW(snapshot,&process))do{if(process.th32ParentProcessID==GetCurrentProcessId()&&_wcsicmp(process.szExeFile,L"clangd.exe")==0){HANDLE child=OpenProcess(PROCESS_TERMINATE,FALSE,process.th32ProcessID);if(child){killed=TerminateProcess(child,42)!=FALSE;CloseHandle(child);}}}while(Process32NextW(snapshot,&process));CloseHandle(snapshot);}
    CHECK(killed);CHECK(wait([](const auto& e){return e.value("kind","")=="status"&&!e.value("ready",true);},3000));
    session.Start(config);CHECK(wait([](const auto& e){return e.value("kind","")=="status"&&e.value("ready",false);}));auto start=GetTickCount64();session.Stop();CHECK(GetTickCount64()-start<2000);
    // A real compile database must supply include directories and conditional macros.
    auto include=root/"headers";fs::create_directories(include);
    auto header=include/"configured.h";
    utils::WriteFileU8(header.wstring(),"#pragma once\n#if PROJECT_FEATURE == 73\nstruct Configured { int enabled_member; };\n#else\nstruct Configured { int disabled_member; };\n#endif\n");
    text="#include <configured.h>\nint main(){Configured x; x. }\n";
    utils::WriteFileU8(file.wstring(),text);
    std::vector<std::string> arguments{utils::WToU8(config.compiler),"-std=c++17","-DPROJECT_FEATURE=73","-I"+utils::WToU8(include.wstring()),key};
    std::ofstream(root/"compile_commands.json")<<json::array({{{"directory",utils::WToU8(root.wstring())},{"file",key},{"arguments",arguments}}}).dump();
    config.database=root.wstring();session.Start(config);
    CHECK(wait([](const auto& e){return e.value("kind","")=="status"&&e.value("ready",false);}));
    offset=text.find("x.")+2;session.Document(key,text,5);session.Request("completion",key,5,offset);
    CHECK(wait([](const auto& e){if(e.value("kind","")!="completion"||e.value("failed",false))return false;bool enabled=false,disabled=false;for(const auto& item:e.at("items")){enabled|=item.at("text")=="enabled_member";disabled|=item.at("text")=="disabled_member";}return enabled&&!disabled;}));
    text="#include <configured.h>\nint main(){Configured x; return x.enabled_member;}\n";offset=text.rfind("enabled_member")+2;
    session.Document(key,text,6);session.Request("definition",key,6,offset);
    CHECK(wait([&](const auto& e){return e.value("kind","")=="definition"&&!e.value("failed",false)&&lsp::FileKey(e.value("target",""))==lsp::FileKey(utils::WToU8(header.wstring()))&&e.at("position").at("line")==2;}));
    session.Close(key);session.Stop();
    fs::remove_all(root);std::cout<<"clangd protocol and real service: "<<passed<<" passed\n";return 0;
}
