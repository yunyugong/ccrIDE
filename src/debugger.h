#pragma once
#include "execution.h"
#include <deque>
#include <mutex>
#include <thread>
#include <map>
struct DebugFrame { int level=0,line=0; std::string function,file; };
struct DebugSnapshot {
    bool active=false, stopped=false;
    unsigned stopRevision=0;
    int line=0;
    std::string status="未启动", file, log, evaluation, error;
    std::vector<std::pair<std::string,std::string>> variables;
    std::vector<DebugFrame> frames;
};
class Debugger {
    std::mutex mutex;
    DebugSnapshot state;
    std::thread worker;
    std::atomic<bool> cancel{false};
    std::atomic<bool> pauseRequested{false};
    HANDLE inferior=nullptr;
    std::string mainThread;
    bool nativePause=false;
    std::deque<std::pair<std::string,std::string>> commands;
    std::map<unsigned,std::pair<std::string,ULONGLONG>> pending;
    unsigned nextToken=0;
    bool startup=true;
    void Log(const std::string& text);
    void Record(const std::string& line);
public:
    ~Debugger() { Stop(); Join(); }
    DebugSnapshot Snapshot();
    bool Start(const std::wstring& gdb, execution::Request target,
               const std::vector<std::pair<std::string,int>>& breakpoints, const std::wstring& inputFile={}, DWORD commandTimeoutMs=10000);
    void Command(const std::string& text, const std::string& role="command");
    void Stop() { cancel.store(true); }
    void Pause() { pauseRequested.store(true); }
    void Join() { if(worker.joinable()) worker.join(); }
    void Step(bool into=false);
    void Continue();
    void Evaluate(const std::string& expression);
    void SelectFrame(int level);
    void SetBreakpoints(const std::vector<std::pair<std::string,int>>& points);
};
