#pragma once
// UTF-8 UI translation shared by the host and separately built plugins.
// Catalog pointers remain valid for the lifetime of the module. No user text is translated.
#include <windows.h>
#include <atomic>
#include <string>
#include <cstring>
#include <map>
#include <mutex>
#include <unordered_map>
namespace i18n {
inline std::atomic<bool> english{false};
inline bool IsEnglish(){return english.load(std::memory_order_relaxed);}
inline const char* ResolveLanguage(const std::string& language,LANGID system=GetUserDefaultUILanguage()) {
    if(language=="en")return "en";
    if(language=="zh-CN")return "zh-CN";
    return PRIMARYLANGID(system)==LANG_CHINESE?"zh-CN":"en";
}
inline void SetLanguage(const std::string& language){english.store(!std::strcmp(ResolveLanguage(language),"en"),std::memory_order_relaxed);}
inline void SetLanguage(const char* language){SetLanguage(std::string(language?language:"auto"));}
inline const char* Language(){return IsEnglish()?"en":"zh-CN";}
inline const char* Tr(const char* text) {
    if(!text||!IsEnglish())return text;
    static const std::unordered_map<std::string,const char*> catalog={
#include "i18n_catalog.inc"
    };
    auto found=catalog.find(text);return found==catalog.end()?text:found->second;
}
// ImGui hashes the suffix after ###. Chinese names retain their existing IDs,
// so docking, positions and sizes survive switching languages without a reset.
inline const char* Window(const char* original) {
    if(!original||!IsEnglish())return original;
    const char* translated=Tr(original);
    if(translated==original){
        static const std::unordered_map<std::string,const char*> reverse=[](){
            std::unordered_map<std::string,const char*> result;
            const std::pair<const char*,const char*> entries[]={
#include "i18n_catalog.inc"
            };
            for(const auto& entry:entries)result.emplace(entry.second,entry.first);
            return result;
        }();
        auto found=reverse.find(original);if(found==reverse.end())return original;
        translated=original;original=found->second;
    }
    static std::mutex lock;static std::map<std::string,std::string> names;
    std::lock_guard<std::mutex> guard(lock);
    auto found=names.emplace(original,std::string(translated)+"###"+original);
    return found.first->second.c_str();
}
inline const wchar_t* Wide(const wchar_t* zh,const wchar_t* en){return IsEnglish()?en:zh;}
}
