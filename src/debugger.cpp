#include "debugger.h"
#include "gdb_mi.h"
#include "utils.h"
#include <algorithm>

// The bundled Windows GDB uses the native ANSI filesystem API, even though
// the editor and MI parser store UTF-8. Convert only at this process boundary.
static std::string GdbNative(const std::wstring& text) {
    int bytes=WideCharToMultiByte(CP_ACP,0,text.data(),(int)text.size(),nullptr,0,nullptr,nullptr);
    std::string result(bytes,0);
    if(bytes) WideCharToMultiByte(CP_ACP,0,text.data(),(int)text.size(),result.data(),bytes,nullptr,nullptr);
    return result;
}
static std::string GdbUtf8(const std::string& text) {
    if(utils::IsValidUtf8(text)) return text;
    int chars=MultiByteToWideChar(CP_ACP,0,text.data(),(int)text.size(),nullptr,0);
    std::wstring result(chars,0);
    if(chars) MultiByteToWideChar(CP_ACP,0,text.data(),(int)text.size(),result.data(),chars);
    return utils::WToU8(result);
}

void Debugger::Log(const std::string& text) {
    state.log+=text;
    if(state.log.size()>65536) state.log.erase(0,state.log.size()-65536);
}
DebugSnapshot Debugger::Snapshot() { std::lock_guard<std::mutex> lock(mutex); return state; }
void Debugger::Command(const std::string& command,const std::string& role) {
    std::lock_guard<std::mutex> lock(mutex);
    if(state.active && commands.size()<128) {
        commands.emplace_back(command,role);
        if(role=="execute") { state.stopped=false; state.status="执行调试命令..."; }
        if(role=="evaluate") state.evaluation.clear();
    }
}
void Debugger::Step(bool into) { if(Snapshot().stopped) Command(into?"-exec-step":"-exec-next","execute"); }
void Debugger::Continue() { if(Snapshot().stopped) Command("-exec-continue","execute"); }
void Debugger::Evaluate(const std::string& expression) { if(Snapshot().stopped) Command("-data-evaluate-expression "+mi::Quote(expression),"evaluate"); }
void Debugger::SelectFrame(int level) { if(Snapshot().stopped) { Command("-stack-select-frame "+std::to_string(level),"select"); } }
void Debugger::SetBreakpoints(const std::vector<std::pair<std::string,int>>& points) {
    if(!Snapshot().stopped) return;
    Command("-interpreter-exec console \"delete breakpoints\"","command");
    for(const auto& point:points) Command("-break-insert "+mi::Quote(GdbNative(utils::U8ToW(point.first))+":"+std::to_string(point.second)),"command");
}

void Debugger::Record(const std::string& line) {
    if(line.empty()||line=="(gdb) "||line=="(gdb)") return;
    std::lock_guard<std::mutex> lock(mutex);
    Log(line+"\n");
    mi::Record record;
    try { record=mi::Parser(line).parse(); }
    catch(const std::exception& e) { state.error=state.status="调试协议异常"; Log(e.what()); cancel.store(true); return; }
    std::string role;
    if(record.kind=='='&&record.cls=="thread-group-started") {
        if(inferior) CloseHandle(inferior);
        DWORD pid=(DWORD)strtoul(record.value.get("pid").text.c_str(),nullptr,10);
        inferior=OpenProcess(PROCESS_ALL_ACCESS,FALSE,pid);
    }
    if(record.kind=='='&&record.cls=="thread-created"&&mainThread.empty()) mainThread=record.value.get("id").text;
    if(record.kind=='^') {
        auto it=pending.find(record.token);
        if(it!=pending.end()) { role=it->second.first; pending.erase(it); }
        if(record.cls=="error") {
            state.status="GDB: "+GdbUtf8(record.value.get("msg").text);
            if(role=="evaluate") state.evaluation=state.status;
            if(role=="setup"||role=="start") { state.error=state.status; cancel.store(true); }
            return;
        }
        if(role=="start") startup=false;
        if(role=="select-thread") {
            const auto& frame=record.value.get("frame");
            state.file=GdbUtf8(frame.get("fullname").text); state.line=atoi(frame.get("line").text.c_str()); ++state.stopRevision;
        }
        if(role=="evaluate") state.evaluation=GdbUtf8(record.value.get("value").text);
        if(role=="variables") {
            state.variables.clear();
            for(const auto& entry:record.value.get("variables").items)
                state.variables.emplace_back(GdbUtf8(entry.get("name").text),GdbUtf8(entry.get("value").text));
        }
        if(role=="stack") {
            state.frames.clear();
            for(const auto& item:record.value.get("stack").items) {
                const auto& frame=item.get("frame"); DebugFrame result;
                result.level=atoi(frame.get("level").text.c_str()); result.line=atoi(frame.get("line").text.c_str());
                result.function=frame.get("func").text; result.file=frame.get("fullname").text;
                if(result.file.empty()) result.file=frame.get("file").text;
                result.file=GdbUtf8(result.file);
                state.frames.push_back(std::move(result));
            }
        }
        if(role=="select") commands.emplace_back("-stack-list-variables --all-values","variables");
    }
    if(record.cls=="running"&&(record.kind=='^'||record.kind=='*')) { state.stopped=false; state.status="调试运行中"; }
    if(record.kind=='*'&&record.cls=="stopped") {
        auto reason=record.value.get("reason").text;
        if(reason=="exited-normally"||reason=="exited"||reason=="exited-signalled") {
            state.stopped=false; state.status="调试程序已结束: "+reason;
            commands.emplace_back("-gdb-exit","exit"); return;
        }
        state.stopped=true; ++state.stopRevision; state.status="已暂停: "+reason;
        const auto& frame=record.value.get("frame");
        state.file=frame.get("fullname").text; if(state.file.empty()) state.file=frame.get("file").text;
        state.file=GdbUtf8(state.file);
        state.line=atoi(frame.get("line").text.c_str());
        if(nativePause&&!mainThread.empty()) commands.emplace_back("-thread-select "+mainThread,"select-thread");
        nativePause=false;
        commands.emplace_back("-stack-list-frames","stack");
        commands.emplace_back("-stack-list-variables --all-values","variables");
    }
}

bool Debugger::Start(const std::wstring& gdb,execution::Request target,
                     const std::vector<std::pair<std::string,int>>& breakpoints,const std::wstring& inputFile,DWORD commandTimeoutMs) {
    if(Snapshot().active) return false;
    Join(); cancel.store(false); pauseRequested.store(false);
    {
        std::lock_guard<std::mutex> lock(mutex);
        state=DebugSnapshot{}; state.active=true; state.status="启动 GDB...";
        commands.clear(); pending.clear(); nextToken=0; startup=true; mainThread.clear(); nativePause=false;
        commands.emplace_back("-gdb-set mi-async on","setup");
        commands.emplace_back("-gdb-set pagination off","setup");
        commands.emplace_back("-gdb-set confirm off","setup");
        commands.emplace_back("-gdb-set print elements 100","setup");
        commands.emplace_back("-file-exec-and-symbols "+mi::Quote(GdbNative(target.exe)),"setup");
        std::string args="-exec-arguments";
        // GDB's native target supports stdin redirection through run arguments.
        for(const auto& argument:target.args) args+=" "+mi::Quote(GdbNative(argument));
        if(!inputFile.empty()) args+=" < "+mi::Quote(GdbNative(inputFile));
        commands.emplace_back(args,"setup");
        for(const auto& point:breakpoints) commands.emplace_back("-break-insert "+mi::Quote(GdbNative(utils::U8ToW(point.first))+":"+std::to_string(point.second)),"setup");
        if(breakpoints.empty()) commands.emplace_back("-break-insert -t main","setup");
        commands.emplace_back("-exec-run","start");
        Log("[GDB] "+utils::WToU8(gdb)+"\n[目标] "+utils::WToU8(execution::CommandLine(target.exe,target.args))+"\n[目录] "+utils::WToU8(target.cwd)+"\n");
    }
    worker=std::thread([this,gdb,target,commandTimeoutMs]() {
        execution::Request request;
        request.exe=gdb; request.args={L"--interpreter=mi2",L"--nx",L"--quiet"};
        request.cwd=target.cwd; request.environment=target.environment;
        request.limits.timeMs=0; request.limits.outputBytes=16*1024*1024;
        request.capture=false; request.interactive=true; request.cancel=&cancel;
        request.pollInput=[this,commandTimeoutMs]() {
            std::lock_guard<std::mutex> lock(mutex);
            if(pauseRequested.exchange(false)&&!state.stopped) {
                if(inferior && DebugBreakProcess(inferior)) nativePause=true;
                else { state.status="暂停失败 (Windows "+std::to_string(GetLastError())+")"; Log(state.status+"\n"); }
            }
            // One outstanding command prevents executing a target after a failed setup.
            for(const auto& entry:pending) if(GetTickCount64()-entry.second.second>commandTimeoutMs) {
                state.error=state.status="GDB 命令超时"; cancel.store(true); return std::string();
            }
            if(!pending.empty()||commands.empty()) return std::string();
            auto command=commands.front(); commands.pop_front();
            unsigned token=++nextToken; pending[token]={command.second,GetTickCount64()};
            return std::to_string(token)+command.first+"\n";
        };
        std::string partial;
        request.onOutput=[this,&partial](const std::string& text,bool err) {
            if(err) { std::lock_guard<std::mutex> lock(mutex); Log(text); return; }
            partial+=text;
            if(partial.size()>1024*1024) { std::lock_guard<std::mutex> lock(mutex); state.error=state.status="GDB 输出行过长"; cancel.store(true); partial.clear(); return; }
            size_t end;
            while((end=partial.find('\n'))!=std::string::npos) {
                std::string line=partial.substr(0,end); partial.erase(0,end+1);
                if(!line.empty()&&line.back()=='\r') line.pop_back();
                Record(line);
            }
        };
        auto result=execution::Run(request);
        std::lock_guard<std::mutex> lock(mutex);
        if(inferior) { CloseHandle(inferior); inferior=nullptr; }
        state.active=false; state.stopped=false;
        if(!state.error.empty()) state.status=state.error;
        else if(!result.started||!result.error.empty()) state.status="GDB 启动或执行失败: "+result.error;
        else if(result.reason==execution::Reason::Exited && result.exitCode!=0) state.status="GDB 异常退出: "+std::to_string(result.exitCode);
        else if(result.reason!=execution::Reason::Exited && state.status.find("失败")==std::string::npos && state.status.find("超时")==std::string::npos && state.status.find("GDB:")==std::string::npos)
            state.status=execution::ReasonText(result.reason);
        Log("[GDB] "+std::string(execution::ReasonText(result.reason))+"，退出码 "+std::to_string(result.exitCode)+"\n");
    });
    return true;
}
