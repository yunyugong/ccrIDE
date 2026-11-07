#pragma once
#include "execution.h"
#include <nlohmann/json.hpp>
#include <thread>
#include <mutex>
#include <deque>
namespace clangd_plugin {
using json=nlohmann::json;
struct Config {std::wstring executable,database,compiler;std::vector<std::string> flags;size_t memoryMiB=1024;bool backgroundIndex=false;};
class Session {
    std::thread worker;std::atomic<bool> cancel{false};std::mutex mutex;
    std::deque<json> commands,events;
    void Event(json event);
public:
    ~Session(){Stop();}
    void Start(Config config);
    void Stop();
    void Document(const std::string& file,const std::string& text,unsigned long long revision);
    void Close(const std::string& file);
    void Request(const std::string& kind,const std::string& file,unsigned long long revision,size_t offset);
    std::vector<json> Poll();
};
}
