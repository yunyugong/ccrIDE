#pragma once
#include <nlohmann/json.hpp>
#include <string>
#include <set>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <stdexcept>
#include "plugin_manager.h"
namespace community {
inline bool Https(const std::string& url){
    if(url.rfind("https://",0)!=0||url.size()>4096||url.find_first_of("\r\n\t ")!=std::string::npos)return false;
    auto end=url.find('/',8);auto authority=url.substr(8,end==std::string::npos?end:end-8);
    return !authority.empty()&&authority.find('@')==std::string::npos;
}
inline bool DownloadUrl(const std::string& url){
    if(Https(url))return true;
    if(url.find_first_of("\r\n\t ")!=std::string::npos)return false;
    for(const char* prefix:{"http://127.0.0.1:","http://localhost:"})if(url.rfind(prefix,0)==0){
        auto end=url.find('/',std::strlen(prefix));auto port=url.substr(std::strlen(prefix),end-std::strlen(prefix));
        return !port.empty()&&port.size()<=5&&std::all_of(port.begin(),port.end(),[](char c){return c>='0'&&c<='9';})&&std::stoul(port)>0&&std::stoul(port)<=65535;
    }
    return false;
}
struct Config {
    std::string repository="https://github.com/yunyugong/ccrIDE";
    std::string releases=repository+"/releases";
    std::string issues=repository+"/issues";
    std::string documentation=repository+"/blob/main/README.en.md";
    std::string market="https://raw.githubusercontent.com/yunyugong/ccrIDE/main/community/marketplace.json";
};
inline Config config;
inline Config ParseConfig(const std::string& text){
    if(text.size()>65536)throw std::runtime_error("Community configuration exceeds 64 KiB");
    auto data=nlohmann::json::parse(text);
    if(data.value("schema",0)!=1)throw std::runtime_error("Unsupported community configuration schema");
    Config result;
    for(auto field:{std::pair<const char*,std::string*>{"repository",&result.repository},{"releases",&result.releases},{"issues",&result.issues},{"documentation",&result.documentation},{"marketplace",&result.market}}){
        if(data.contains(field.first))*field.second=data.at(field.first).get<std::string>();
        if(!field.second->empty()&&!Https(*field.second))throw std::runtime_error(std::string("Expected an HTTPS URL for ")+field.first);
    }
    return result;
}
inline bool Digest(const std::string& value){return value.size()==64&&std::all_of(value.begin(),value.end(),[](unsigned char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F');});}
inline bool Id(const std::string& id){return !id.empty()&&id.size()<=80&&std::all_of(id.begin(),id.end(),[](unsigned char c){return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_';});}
inline std::vector<MarketItem> ParseMarket(const std::string& text){
    if(text.size()>2*1024*1024)throw std::runtime_error("Marketplace catalog exceeds 2 MiB");
    auto data=nlohmann::json::parse(text);
    // Legacy array catalogs remain readable. New catalogs use a versioned envelope.
    if(data.is_object()){
        if(data.value("schema",0)!=1)throw std::runtime_error("Unsupported marketplace schema");
        data=data.at("plugins");
    }
    if(!data.is_array()||data.size()>500)throw std::runtime_error("Expected at most 500 marketplace entries");
    std::vector<MarketItem> result;std::set<std::string> ids;
    for(const auto& item:data){
        MarketItem out;
        out.id=item.value("id",std::string{});out.name=item.value("name",out.id);out.version=item.value("version",std::string{});
        out.description=item.value("description",std::string{});out.author=item.value("author",std::string{});
        out.entry=item.value("entry",std::string{});out.dllUrl=item.value("dllUrl",std::string{});out.packageUrl=item.value("packageUrl",std::string{});
        out.sha256=item.value("sha256",std::string{});out.apiVersion=item.value("apiVersion",1u);out.taskCoordinatorVersion=item.value("taskCoordinatorVersion",0u);
        if(!Id(out.id)||!ids.insert([&](){auto key=out.id;std::transform(key.begin(),key.end(),key.begin(),[](unsigned char c){return char(std::tolower(c));});return key;}()).second)throw std::runtime_error("Invalid or duplicate plugin ID");
        if(out.entry.size()<5||out.entry.find_first_of("/\\:")!=std::string::npos||out.entry.substr(out.entry.size()-4)!=".dll")throw std::runtime_error("Invalid plugin DLL entry");
        if(out.version.empty()||out.version.size()>64||out.name.size()>256||out.description.size()>16384||out.author.size()>256)throw std::runtime_error("Invalid marketplace metadata");
        if(out.apiVersion<1||out.apiVersion>CCRIDE_PLUGIN_API_VERSION)throw std::runtime_error("Unsupported plugin API version");
        if(out.packageUrl.empty()==out.dllUrl.empty())throw std::runtime_error("Specify exactly one of packageUrl and dllUrl");
        if(!DownloadUrl(out.packageUrl.empty()?out.dllUrl:out.packageUrl))throw std::runtime_error("Marketplace downloads require HTTPS");
        if(!Digest(out.sha256))throw std::runtime_error("Marketplace downloads require a SHA-256 digest");
        result.push_back(std::move(out));
    }
    return result;
}
}
