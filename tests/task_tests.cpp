#include "execution.h"
#include "utils.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <stdexcept>
#include <nlohmann/json.hpp>
namespace fs=std::filesystem;
static int failures=0,passed=0;
#define CHECK(x) do{if(x)++passed;else{++failures;std::cerr<<"FAIL "<<__LINE__<<": " #x "\n";}}while(0)
template<class F> bool Wait(F f,DWORD ms=4000){auto start=GetTickCount64();while(!f()){if(GetTickCount64()-start>ms)return false;Sleep(10);}return true;}
std::wstring Self(){wchar_t path[32768]{};GetModuleFileNameW(nullptr,path,32768);return path;}
std::wstring Env(const wchar_t* key){wchar_t text[32768]{};GetEnvironmentVariableW(key,text,32768);return text;}
int wmain(int argc,wchar_t** argv){
 if(argc>1&&std::wstring(argv[1])==L"--child"){
  std::ofstream("same-name.txt")<<utils::WToU8(Env(L"CCRIDE_TASK_ID"));
  std::cout<<utils::WToU8(Env(L"CCRIDE_TASK_ID"))<<"\n"<<utils::WToU8(Env(L"TEMP"))<<"\n";
  return 0;
 }
 if(argc>1&&std::wstring(argv[1])==L"--owner"){
  execution::Request r;r.exe=(fs::path(utils::GetExeDir())/"execution_fixture.exe").wstring();
  r.args={L"sleep"};r.taskRoot=argv[2];r.limits.timeMs=30000;
  r.onStarted=[&](DWORD pid){std::ofstream record{fs::path(argv[3])};record<<pid;};
  return execution::Run(r).started?0:1;
 }
 auto root=fs::temp_directory_path()/(L"ccrIDE 任务测试 "+std::to_wstring(GetCurrentProcessId()));
 fs::create_directories(root);SetEnvironmentVariableW(L"CCRIDE_TASK_ROOT",(root/"tasks").c_str());
 execution::Request base;base.exe=(fs::path(utils::GetExeDir())/"execution_fixture.exe").wstring();base.cwd=root.wstring();
 base.args={L"sleep"};base.limits.timeMs=15000;
 std::atomic<int> started{0};std::atomic<bool> release{false};
 auto hold=base;hold.cancel=&release;hold.onStarted=[&](DWORD){++started;};
 execution::Result first,second;
 std::thread a([&]{first=execution::Run(hold);}),b([&]{second=execution::Run(hold);});
 CHECK(Wait([&]{return started==2;}));
 // A third foreground task must remain queued and can be cancelled without starting.
 auto queued=base;std::string queuedId;std::atomic<bool> seenQueue{false};execution::Result cancelled;
 queued.onState=[&](const execution::TaskInfo& info){if(info.state=="queued"){queuedId=info.task.id;seenQueue=true;}};
 std::thread c([&]{cancelled=execution::Run(queued);});
 CHECK(Wait([&]{return seenQueue.load();}));Sleep(100);
 CHECK(execution::CancelTask(queuedId));c.join();
 CHECK(!cancelled.started&&cancelled.reason==execution::Reason::Cancelled);
 CHECK(!fs::exists(fs::path(cancelled.task.directory)/"stdout.bin"));
 auto timeout=base;timeout.queueTimeoutMs=80;auto timed=execution::Run(timeout);
 CHECK(!timed.started&&timed.reason==execution::Reason::QueueTimeout&&timed.queuedMs>=80);
 // Background work has its own single slot, independent of foreground occupancy.
 auto bg=base;bg.lane=execution::Lane::Background;std::atomic<bool> bgStarted{false},bgStop{false};
 bg.cancel=&bgStop;bg.onStarted=[&](DWORD){bgStarted=true;};execution::Result background;
 std::thread d([&]{background=execution::Run(bg);});CHECK(Wait([&]{return bgStarted.load();}));
 // Services use two separate slots, even while all foreground/background slots are held.
 std::atomic<int> services{0};std::atomic<bool> serviceStop{false};
 auto service=base;service.lane=execution::Lane::Service;service.cancel=&serviceStop;service.onStarted=[&](DWORD){++services;};
 execution::Result sr1,sr2;std::thread svc1([&]{sr1=execution::Run(service);}),svc2([&]{sr2=execution::Run(service);});
 CHECK(Wait([&]{return services==2;}));
 auto serviceQueued=service;serviceQueued.cancel=nullptr;serviceQueued.queueTimeoutMs=80;
 CHECK(execution::Run(serviceQueued).reason==execution::Reason::QueueTimeout);
 serviceStop=true;svc1.join();svc2.join();CHECK(sr1.reason==execution::Reason::Cancelled&&sr2.reason==execution::Reason::Cancelled);
 auto bgQueued=bg;bgQueued.cancel=nullptr;bgQueued.queueTimeoutMs=80;
 auto bgTimed=execution::Run(bgQueued);CHECK(!bgTimed.started&&bgTimed.reason==execution::Reason::QueueTimeout);
 bgStop=true;d.join();CHECK(background.reason==execution::Reason::Cancelled);
 // Waiting time must not consume the program's execution deadline.
 execution::Result afterQueue;auto shortRun=base;shortRun.args={L"exit7"};shortRun.limits.timeMs=1000;
 std::atomic<bool> shortQueued{false};shortRun.onState=[&](auto& s){if(s.state=="queued")shortQueued=true;};
 std::thread e([&]{afterQueue=execution::Run(shortRun);});
 CHECK(Wait([&]{return shortQueued.load();}));Sleep(1100);release=true;a.join();b.join();e.join();
 CHECK(afterQueue.started&&afterQueue.reason==execution::Reason::Exited&&afterQueue.exitCode==7&&afterQueue.queuedMs>=1000);
 CHECK(first.reason==execution::Reason::Cancelled&&second.reason==execution::Reason::Cancelled);
 // Concurrent children may use the same relative filename safely.
 auto isolated=base;isolated.exe=Self();isolated.args={L"--child"};isolated.isolateWorkingDirectory=true;
 execution::Result one,two;std::thread f([&]{one=execution::Run(isolated);});
 two=execution::Run(isolated);f.join();
 CHECK(one.reason==execution::Reason::Exited&&two.reason==execution::Reason::Exited);
 CHECK(one.task.id!=two.task.id&&one.cwd!=two.cwd);
 std::string content;utils::ReadFileU8((fs::path(one.cwd)/"same-name.txt").wstring(),content);CHECK(content==one.task.id);
 utils::ReadFileU8((fs::path(two.cwd)/"same-name.txt").wstring(),content);CHECK(content==two.task.id);
 CHECK(one.out.find(one.task.id)!=std::string::npos&&one.out.find(utils::WToU8((fs::path(one.task.directory)/"tmp").wstring()))!=std::string::npos);
 CHECK(!fs::exists(root/"same-name.txt"));
 // Explicit working directories remain supported.
 auto preserve=base;preserve.args={L"touch"};auto preserved=execution::Run(preserve);
 CHECK(preserved.exitCode==0&&fs::exists(root/"started-marker"));
 auto reused=preserve;reused.task=preserved.task;auto duplicate=execution::Run(reused);
 CHECK(!duplicate.started&&duplicate.reason==execution::Reason::LaunchFailed);
 auto privateRun=isolated;privateRun.logOutput=false;privateRun.input="private input";privateRun.environment[L"SECRET_KEY"]=L"private-key";
 auto secret=execution::Run(privateRun);CHECK(!fs::exists(fs::path(secret.task.directory)/"stdout.bin"));
 utils::ReadFileU8((fs::path(secret.task.directory)/"task.json").wstring(),content);
 CHECK(content.find("private input")==std::string::npos&&content.find("private-key")==std::string::npos);
 auto faulty=base;faulty.onStarted=[](DWORD){throw std::runtime_error("test");};
 auto failed=execution::Run(faulty);CHECK(failed.reason==execution::Reason::IoError&&failed.elapsedMs<2000);
 auto outputFault=isolated;outputFault.onOutput=[](auto&,bool){throw std::runtime_error("test");};
 CHECK(execution::Run(outputFault).reason==execution::Reason::IoError);
 auto inputFault=base;inputFault.interactive=true;inputFault.pollInput=[]()->std::string{throw std::runtime_error("test");};
 CHECK(execution::Run(inputFault).reason==execution::Reason::IoError);
 auto invalid=base;invalid.taskRoot=(root/"started-marker").wstring();CHECK(!execution::Run(invalid).started);
 // A second coordinator process shares slots. Killing it releases its abandoned mutex and Job.
 release=false;started=0;std::thread g([&]{first=execution::Run(hold);});
 CHECK(Wait([&]{return started==1;}));
 auto marker=root/"external-child.pid";auto exe=Self();
 auto command=execution::CommandLine(exe,{L"--owner",(root/"tasks").wstring(),marker.wstring()});
 STARTUPINFOW si{sizeof(si)};PROCESS_INFORMATION pi{};
 bool launched=CreateProcessW(exe.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&si,&pi);
 CHECK(launched);HANDLE child=nullptr;
 if(launched){
  CloseHandle(pi.hThread);CHECK(Wait([&]{return fs::exists(marker)&&fs::file_size(marker)>0;}));
  DWORD pid=0;{std::ifstream record(marker);record>>pid;}
  child=OpenProcess(SYNCHRONIZE,FALSE,pid);CHECK(child!=nullptr);
  auto probe=base;probe.queueTimeoutMs=80;CHECK(execution::Run(probe).reason==execution::Reason::QueueTimeout);
  TerminateProcess(pi.hProcess,99);CHECK(WaitForSingleObject(pi.hProcess,3000)==WAIT_OBJECT_0);CloseHandle(pi.hProcess);
  if(child){CHECK(WaitForSingleObject(child,3000)==WAIT_OBJECT_0);CloseHandle(child);}
  auto recovered=preserve;recovered.queueTimeoutMs=500;CHECK(execution::Run(recovered).started);
 }
 release=true;g.join();
 auto records=execution::ListTasks();bool interrupted=false;for(auto& record:records)if(record.state=="interrupted")interrupted=true;
 CHECK(interrupted);CHECK(records.size()>=15);
 for(auto& record:records)CHECK(!record.active);
 fs::remove_all(root);std::cout<<passed<<" passed, "<<failures<<" failed\n";return failures?1:0;
}
