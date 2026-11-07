#include "debugger.h"
#include "gdb_mi.h"
#include "utils.h"
#include <filesystem>
#include <iostream>
#include <functional>

static int passed=0,failed=0;
#define CHECK(x) do { if(x) ++passed; else { ++failed; std::cout<<"FAIL "<<__LINE__<<": " #x "\n"; } } while(0)
int main() {
    auto record=mi::Parser("12^done,stack=[frame={level=\"0\",func=\"f\",fullname=\"C:\\\\a.cpp\",line=\"9\"},frame={level=\"1\",func=\"main\"}]").parse();
    CHECK(record.token==12 && record.value.get("stack").items.size()==2);
    CHECK(record.value.get("stack").items[0].get("frame").get("fullname").text=="C:\\a.cpp");
    CHECK(mi::Parser("~\"hello\\n\\344\\270\\255\"").parse().value.text=="hello\n中");
    CHECK(mi::Parser("^done,value="+mi::Quote("a\"b\\c\n")).parse().value.get("value").text=="a\"b\\c\n");
    bool rejected=false; try { mi::Parser("^done,stack=[frame={level=\"0\"").parse(); } catch(...) { rejected=true; } CHECK(rejected);
    namespace fs=std::filesystem;
    auto compiler=fs::path(L"E:/rred/mingw64/bin/g++.exe");
    auto gdb=compiler.parent_path()/L"gdb.exe";
    if(!fs::exists(compiler)||!fs::exists(gdb)) { std::cout<<"GDB integration skipped: toolchain not installed\n"; return failed?1:77; }
    auto root=fs::temp_directory_path()/(L"ccrIDE-debug-测试 "+std::to_wstring(GetCurrentProcessId())); fs::create_directories(root);
    auto source=root/L"main.cpp", exe=root/L"debug.exe", input=root/L"input.txt";
    utils::WriteFileU8(source.wstring(),"#include <cstdio>\nint twice(int x){\n int y=x*2;\n return y;\n}\nint main(){\n int input=0;\n std::scanf(\"%d\",&input);\n int value=twice(input);\n std::printf(\"debug-result=%d\\n\",value);\n return 0;\n}\n");
    utils::WriteFileU8(input.wstring(),"21\n");
    auto build=utils::RunProcess(compiler.wstring(),{source.wstring(),L"-g",L"-O0",L"-o",exe.wstring()},root.wstring(),(root/L"out").wstring(),(root/L"err").wstring(),15000,true);
    CHECK(build.ok&&build.exitCode==0);
    if(!build.ok||build.exitCode) { std::cout<<build.err; return 1; }
    Debugger debugger;
    execution::Request target; target.exe=exe.wstring(); target.cwd=root.wstring();
    wchar_t path[32768]{}; GetEnvironmentVariableW(L"PATH",path,32768);
    target.environment[L"PATH"]=compiler.parent_path().wstring()+L";"+path;
    auto wait=[&](std::function<bool(const DebugSnapshot&)> predicate) {
        auto start=GetTickCount64();
        while(GetTickCount64()-start<7000) { auto state=debugger.Snapshot(); if(predicate(state)) return true; if(!state.active) break; Sleep(20); }
        std::cout<<debugger.Snapshot().status<<"\n"<<debugger.Snapshot().log<<"\n"; ++failed; return false;
    };
    CHECK(debugger.Start(gdb.wstring(),target,{{utils::WToU8(source.wstring()),9}},input.wstring()));
    bool paused=wait([](const DebugSnapshot& s){ return s.stopped&&s.line==9&&!s.variables.empty()&&!s.frames.empty(); });
    CHECK(paused);
    if(paused) CHECK(debugger.Snapshot().file==utils::WToU8(source.wstring()));
    if(paused) {
        debugger.Evaluate("input"); CHECK(wait([](const DebugSnapshot& s){ return s.evaluation=="21"; }));
        debugger.Step(true); CHECK(wait([](const DebugSnapshot& s){ return s.stopped&&s.line==3&&s.frames.size()>=2; }));
        debugger.Step(); CHECK(wait([](const DebugSnapshot& s){ return s.stopped&&s.line==4; }));
        debugger.Evaluate("y"); CHECK(wait([](const DebugSnapshot& s){ return s.evaluation=="42"; }));
        debugger.SelectFrame(1); CHECK(wait([](const DebugSnapshot& s){ for(const auto& v:s.variables) if(v.first=="input"&&v.second=="21") return true; return false; }));
        debugger.SelectFrame(0);
        debugger.SetBreakpoints({});
        debugger.Continue(); CHECK(wait([](const DebugSnapshot& s){ return !s.active; }));
        CHECK(debugger.Snapshot().log.find("debug-result=42")!=std::string::npos);
    }
    debugger.Stop(); debugger.Join();
    CHECK(!debugger.Snapshot().active);
    // A missing debugger and an invalid target must fail clearly and remain stoppable.
    CHECK(debugger.Start((root/L"missing-gdb.exe").wstring(),target,{}));
    Sleep(100); debugger.Join(); CHECK(!debugger.Snapshot().active); CHECK(debugger.Snapshot().status.find("失败")!=std::string::npos);
    target.exe=root.wstring()+L"\\missing.exe";
    CHECK(debugger.Start(gdb.wstring(),target,{}));
    CHECK(wait([](const DebugSnapshot& s){ return !s.active; }));
    debugger.Stop(); debugger.Join(); CHECK(debugger.Snapshot().status.find("GDB:")!=std::string::npos);
    auto fakeGdb=fs::path(utils::GetExeDir())/L"execution_fixture.exe";
    for(auto behavior:{L"protocol",L"hang"}) {
        target.environment[L"CCRIDE_GDB_FIXTURE"]=behavior;
        CHECK(debugger.Start(fakeGdb.wstring(),target,{}, {},300));
        CHECK(wait([](const DebugSnapshot& s){return !s.active;}));
        debugger.Join();
        CHECK(debugger.Snapshot().status.find(behavior==std::wstring(L"protocol")?"协议异常":"超时")!=std::string::npos);
    }
    target.environment.erase(L"CCRIDE_GDB_FIXTURE"); target.exe=exe.wstring();
    utils::WriteFileU8(source.wstring(),"int main(){\n volatile unsigned value=0;\n while(true) ++value;\n}\n");
    build=utils::RunProcess(compiler.wstring(),{source.wstring(),L"-g",L"-O0",L"-o",exe.wstring()},root.wstring(),(root/L"out").wstring(),(root/L"err").wstring(),15000,true);
    CHECK(build.exitCode==0);
    CHECK(debugger.Start(gdb.wstring(),target,{}));
    if(wait([](const DebugSnapshot& s){return s.stopped;})) {
        debugger.Continue(); CHECK(wait([](const DebugSnapshot& s){return s.active&&!s.stopped;}));
        Sleep(100); debugger.Pause();
        CHECK(wait([](const DebugSnapshot& s){return s.stopped;}));
    }
    auto stopStart=GetTickCount64(); debugger.Stop(); debugger.Join();
    CHECK(GetTickCount64()-stopStart<1800 && !debugger.Snapshot().active);
    auto expected=fs::temp_directory_path(); if(expected.filename().empty()) expected=expected.parent_path();
    if(root.parent_path()==expected&&root.filename().wstring().find(L"ccrIDE-debug-")==0) { std::error_code error; fs::remove_all(root,error); CHECK(!error); }
    std::cout<<"Debugger checks: "<<passed<<" passed, "<<failed<<" failed\n";
    return failed?1:0;
}
