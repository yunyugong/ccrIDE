#pragma once
#include <string>
#include <vector>
#include <cstdint>
namespace recovery {
struct Tab {std::string name,path,text,baseline;size_t cursor=0,anchor=0;bool dirty=false;int encoding=0;float scrollX=0,scrollY=0;std::vector<int> breakpoints;};
struct Snapshot {std::vector<Tab> tabs;int active=-1;};
bool Save(const std::wstring& directory,const Snapshot& snapshot,std::string& error);
bool Load(const std::wstring& directory,Snapshot& snapshot,std::string& error);
}
