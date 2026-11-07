#include "../plugins/ai-assistant/ai_assist.cpp"
#include <iostream>
#include <tlhelp32.h>
static std::string dataDir;
static int commands=0,passes=0;
static const char* Data(){return dataDir.c_str();}
static void Log(const char*){}
static void Command(const char*,const char*,void(*)()){++commands;}
static void SelectionSink(PluginTextSink sink,void* user){const std::string text="int selectedValue=42;";sink(user,text.data(),text.size());}
static void Insert(const char*){}
static void Dock(const char*,int){}
#define CHECK(value) do {if(!(value)){std::cerr<<"FAIL "<<__LINE__<<": "<<#value<<"\n";return 1;}++passes;}while(0)
int wmain(int argc,wchar_t** argv) {
    if(argc!=2)return 2;dataDir=utils::WToU8(argv[1]);
    ImGui::CreateContext();PluginApi api{};api.apiVersion=2;api.structSize=sizeof(api);api.GetDataDir=Data;api.Log=Log;api.RegisterCommand=Command;api.GetSelection=SelectionSink;api.InsertText=Insert;api.DockNextWindow=Dock;
    ImGui::GetAllocatorFunctions(&api.Allocate,&api.Deallocate,&api.allocatorUser);
    Plugin_OnLoadV2(ImGui::GetCurrentContext(),&api);
    CHECK(commands==2);CHECK(settings.enabled);CHECK(std::filesystem::is_regular_file(settingsPath));CHECK(settings.apiKey=="plugin-test-key");
    CHECK(Selection().find("42")!=std::string::npos);
    state.TestConnection();state.worker.join();if(!state.error.empty())std::cerr<<state.error<<"\n";CHECK(!state.busy && state.result.find("mock-model")!=std::string::npos);
    state.useStream=false;state.Send("normal",Selection());state.worker.join();
    CHECK(state.error.empty());CHECK(state.result==u8"完整回复：你好");CHECK(state.history.size()==2);
    state.useStream=true;state.Send("stream",Selection());state.worker.join();
    CHECK(state.error.empty());CHECK(state.result==u8"流式回复：你好");CHECK(state.history.size()==4);
    auto waitForPiece=[&](){
        auto start=GetTickCount64();
        while(GetTickCount64()-start<3000){
            {std::lock_guard<std::mutex> lock(state.mtx);if(!state.result.empty())return true;}
            if(!state.busy)return false;Sleep(10);
        }
        return false;
    };
    state.Send("slow","");CHECK(waitForPiece());state.Stop();state.worker.join();CHECK(!state.busy);
    auto historyBefore=state.history.size();
    state.Send("slow","");
    bool killed=false;
    for(int attempt=0;attempt<100 && !killed;++attempt) {
        Sleep(10);
        HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);PROCESSENTRY32W process{};process.dwSize=sizeof(process);
        if(snapshot!=INVALID_HANDLE_VALUE) {
            if(Process32FirstW(snapshot,&process))do {
                if(process.th32ParentProcessID==GetCurrentProcessId() && _wcsicmp(process.szExeFile,L"ai-service.exe")==0) {
                    HANDLE child=OpenProcess(PROCESS_TERMINATE,FALSE,process.th32ProcessID);
                    if(child){killed=TerminateProcess(child,42)!=FALSE;CloseHandle(child);}
                }
            }while(Process32NextW(snapshot,&process));
            CloseHandle(snapshot);
        }
    }
    state.worker.join();CHECK(killed);CHECK(!state.error.empty());CHECK(state.history.size()==historyBefore);
    state.useStream=false;state.Send("normal","");state.worker.join();CHECK(state.error.empty());CHECK(state.history.size()==historyBefore+2);
    state.useStream=true;
    state.Send("slow","");CHECK(waitForPiece());auto start=GetTickCount64();Plugin_OnUnload();
    CHECK(!state.worker.joinable());CHECK(GetTickCount64()-start<3000);
    ImGui::DestroyContext();std::cout<<"AI plugin: "<<passes<<" passed\n";
    return 0;
}
