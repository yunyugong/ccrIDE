#pragma once
#include "symbol_index.h"
#include "code_model.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
namespace background {
struct HeaderResult {uint64_t signature=0;SymbolIndex::HeaderUnit unit;CodeModel model;};
class Index {
    struct Job {uint64_t uid=0,signature=0;std::string text;std::wstring path,compiler;std::shared_ptr<std::atomic<bool>> cancel=std::make_shared<std::atomic<bool>>(false);};
    std::mutex mutex;std::condition_variable wake;std::thread worker;bool enabled=false,stopping=false;
    std::deque<Job> queue;Job active;bool working=false;
    std::map<uint64_t,std::shared_ptr<HeaderResult>> results;
    std::map<std::wstring,std::vector<std::wstring>> directories;
    void Run();
    void Queue(Job job);
public:
    static Index& I();
    ~Index(){Stop();}
    void Enable();bool Enabled() const{return enabled;}
    void Stop();
    void Cancel(uint64_t uid);
    bool Directories(const std::wstring& compiler,std::vector<std::wstring>& out);
    std::shared_ptr<HeaderResult> Headers(uint64_t uid,uint64_t signature,const std::string& text,const std::wstring& path,const std::wstring& compiler);
};
}
