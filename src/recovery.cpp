#include "recovery.h"
#include "utils.h"
#include <filesystem>
#include <nlohmann/json.hpp>
#include <windows.h>
namespace recovery {
using nlohmann::json;
static json Encode(const Snapshot& snapshot) {
    json tabs=json::array();
    for(const auto& t:snapshot.tabs)tabs.push_back({{"name",t.name},{"path",t.path},{"text",t.text},{"baseline",t.baseline},{"cursor",t.cursor},{"anchor",t.anchor},{"dirty",t.dirty},{"encoding",t.encoding},{"scrollX",t.scrollX},{"scrollY",t.scrollY},{"breakpoints",t.breakpoints}});
    return {{"schema",1},{"active",snapshot.active},{"tabs",tabs}};
}
bool Save(const std::wstring& directory,const Snapshot& snapshot,std::string& error) {
    std::wstring temp;
    try {
        std::filesystem::create_directories(directory);
        auto file=directory+L"\\workspace.json";temp=file+L".tmp-"+std::to_wstring(GetCurrentProcessId());
        auto text=Encode(snapshot).dump();
        HANDLE handle=CreateFileW(temp.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(handle==INVALID_HANDLE_VALUE)throw std::runtime_error("无法创建恢复快照");
        bool ok=true;size_t pos=0;
        while(pos<text.size()){DWORD written=0;DWORD amount=DWORD(std::min<size_t>(text.size()-pos,1024*1024));if(!WriteFile(handle,text.data()+pos,amount,&written,nullptr)||written!=amount){ok=false;break;}pos+=written;}
        if(ok)ok=FlushFileBuffers(handle)!=FALSE;CloseHandle(handle);
        if(!ok)throw std::runtime_error("恢复快照写入失败，旧快照已保留");
        // Keep the previous committed generation in case disk/content corruption is detected.
        if(GetFileAttributesW(file.c_str())!=INVALID_FILE_ATTRIBUTES && !CopyFileW(file.c_str(),(file+L".previous").c_str(),FALSE))throw std::runtime_error("无法保留上一代恢复快照");
        if(!MoveFileExW(temp.c_str(),file.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))throw std::runtime_error("恢复快照提交失败");
        error.clear();return true;
    }catch(const std::exception& e){if(!temp.empty())DeleteFileW(temp.c_str());error=e.what();return false;}
}
bool Load(const std::wstring& directory,Snapshot& snapshot,std::string& error) {
    bool found=false;
    for(const auto& suffix:{L"",L".previous"})try {
        auto file=directory+L"\\workspace.json"+suffix;
        if(GetFileAttributesW(file.c_str())==INVALID_FILE_ATTRIBUTES)continue;found=true;
        std::string text;if(!utils::ReadFileU8(file,text))throw std::runtime_error("read failed");
        auto js=json::parse(text);if(js.at("schema")!=1||!js.at("tabs").is_array())throw std::runtime_error("schema");
        Snapshot result;result.active=js.at("active").get<int>();
        for(const auto& item:js.at("tabs")){Tab t;t.name=item.at("name");t.path=item.at("path");t.text=item.at("text");t.baseline=item.at("baseline");t.cursor=std::min(item.at("cursor").get<size_t>(),t.text.size());t.anchor=std::min(item.at("anchor").get<size_t>(),t.text.size());t.dirty=item.at("dirty");t.encoding=item.at("encoding");t.scrollX=item.value("scrollX",0.0f);t.scrollY=item.value("scrollY",0.0f);t.breakpoints=item.value("breakpoints",std::vector<int>{});result.tabs.push_back(std::move(t));}
        snapshot=std::move(result);error=suffix[0]?"主快照损坏，已读取上一代恢复快照":"";return true;
    }catch(...) {}
    if(found)error="恢复快照损坏，原文件保留，未覆盖任何源码";return false;
}
}
