#include "contest.h"
#include "recovery.h"
#include "utils.h"
#include <filesystem>
#include <iostream>
#include <fstream>
#include <nlohmann/json.hpp>
namespace fs=std::filesystem;
#define CHECK(x) do{if(!(x)){std::cerr<<"FAIL "<<__LINE__<<": "<<#x<<"\n";return 1;}++passed;}while(0)
int wmain(int argc,wchar_t** argv){
 int passed=0;if(argc!=2)return 2;auto root=fs::temp_directory_path()/(L"ccride-workflow-中文 "+std::to_wstring(GetCurrentProcessId()));fs::create_directories(root);
 recovery::Snapshot snapshot;snapshot.active=1;snapshot.tabs.push_back({"saved","original.cpp","int changed;","int old;",5,2,true});snapshot.tabs.push_back({"untitled","","","",0,0,true});std::string error;
 CHECK(recovery::Save((root/"recovery").wstring(),snapshot,error));recovery::Snapshot restored;CHECK(recovery::Load((root/"recovery").wstring(),restored,error));CHECK(restored.tabs.size()==2&&restored.tabs[1].text.empty()&&restored.active==1&&restored.tabs[0].anchor==2);
 auto next=snapshot;next.tabs[0].text="second";CHECK(recovery::Save((root/"recovery").wstring(),next,error));utils::WriteFileU8((root/"recovery/workspace.json").wstring(),"{broken");CHECK(recovery::Load((root/"recovery").wstring(),restored,error));CHECK(restored.tabs[0].text=="int changed;"&&!error.empty());
 // A failed replacement after corruption must not clobber the valid backup.
 HANDLE locked=CreateFileW((root/"recovery/workspace.json").c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);
 CHECK(locked!=INVALID_HANDLE_VALUE);CHECK(!recovery::Save((root/"recovery").wstring(),next,error));CloseHandle(locked);
 CHECK(recovery::Load((root/"recovery").wstring(),restored,error)&&restored.tabs[0].text=="int changed;");
 CHECK(recovery::Save((root/"recovery").wstring(),next,error));utils::WriteFileU8((root/"recovery/workspace.json").wstring(),"{bad again");
 CHECK(recovery::Load((root/"recovery").wstring(),restored,error)&&restored.tabs[0].text=="int changed;");
 auto unicode=snapshot;unicode.tabs[0].text=u8"中\nx";unicode.tabs[0].cursor=2;unicode.tabs[0].breakpoints={-1,1,1,2,8};unicode.tabs[0].scrollX=-10;
 CHECK(recovery::Save((root/"unicode").wstring(),unicode,error));CHECK(recovery::Load((root/"unicode").wstring(),restored,error));CHECK(restored.tabs[0].cursor==0&&restored.tabs[0].breakpoints==std::vector<int>({1,2})&&restored.tabs[0].scrollX==0);
 utils::WriteFileU8((root/"not-a-directory").wstring(),"x");CHECK(!recovery::Save((root/"not-a-directory").wstring(),snapshot,error));
 std::string diff;CHECK(contest::Equal("a b\n","a\tb ",contest::Compare::Tokens,diff));CHECK(!contest::Equal("a b\n","a\tb ",contest::Compare::Exact,diff));CHECK(!contest::Equal("1.0","1",contest::Compare::Tokens,diff));CHECK(!contest::Equal(std::string(1,char(0xff)),std::string(1,char(0xfe)),contest::Compare::Exact,diff));CHECK((!nlohmann::json{{"difference",diff}}.dump().empty()));
 std::vector<contest::Group> groups{{"a",30,{}},{"b",70,{"a"}}};std::vector<contest::Result> results{{"1","WA","","","","a"},{"2","AC","","","","b"}};CHECK(contest::Score(groups,results)==0);results[0].verdict="AC";CHECK(contest::Score(groups,results)==100);groups[0].requires={"b"};CHECK(contest::Score(groups,results)==0);
 contest::Config config;config.compiler=argv[1];config.archive=(root/"runs").wstring();config.solution=(root/"solution.cpp").wstring();config.flags={L"-std=c++17",L"-O0"};config.limits={300,64*1024*1024,65536};config.groups={{"default",100,{}}};
 utils::WriteFileU8((root/"local.h").wstring(),"#pragma once\n");
 utils::WriteFileU8(config.solution,"#include \"local.h\"\n#include <iostream>\nint main(){int n;std::cin>>n;if(n<0)return 7;if(n==4){auto p=new char[256*1024*1024];for(int i=0;i<256*1024*1024;++i)p[i]=1;}if(n==2)for(;;){}if(n==3)for(;;)std::cout<<\"xxxxxxxxxxxxxxxx\";std::cout<<n;}\n");
 config.cases={{"ok","0\n","0"},{"wrong","1\n","9"},{"runtime","-1\n",""},{"timeout","2\n",""},{"flood","3\n",""},{"memory","4\n",""}};
 contest::Runner runner;auto run=[&](){runner.Start(config);auto start=GetTickCount64();while(runner.Snapshot().running&&GetTickCount64()-start<30000)Sleep(15);auto state=runner.Snapshot();runner.Stop();return state;};
 auto state=run();CHECK(state.results.size()==6);const char* expected[]={"AC","WA","RE","TLE","OLE","MLE"};for(int i=0;i<6;++i)CHECK(state.results[i].verdict==expected[i]);CHECK(state.results[2].exitCode==7&&!state.results[2].termination.empty());CHECK(fs::is_regular_file(fs::u8path(state.archive)/"case-2/input.txt"));CHECK(fs::is_regular_file(fs::u8path(state.archive)/"run.json"));
 std::string report;CHECK(utils::ReadFileU8((fs::u8path(state.archive)/"run.json").wstring(),report));auto record=nlohmann::json::parse(report);CHECK(record.at("status")=="completed");CHECK(record.at("casePlan").size()==6&&record.at("groups").size()==1);CHECK(record["programs"][0]["sha256"].get<std::string>().size()==64);
 std::string sourceSnapshot;CHECK(utils::ReadFileU8((fs::u8path(state.archive)/"solution.cpp.snapshot").wstring(),sourceSnapshot));CHECK(record["programs"][0]["sha256"]==utils::Sha256Hex(sourceSnapshot));
 std::string processRecord;CHECK(utils::ReadFileU8((fs::u8path(state.archive)/"case-3/processes.json").wstring(),processRecord));CHECK(nlohmann::json::parse(processRecord)[0]["exitCode"]==7);
 auto arguments=record["programs"][0]["arguments"].get<std::vector<std::string>>();CHECK(std::find(arguments.begin(),arguments.end(),utils::WToU8(config.solution))==arguments.end());CHECK(fs::is_regular_file(fs::u8path(state.archive)/"case-1/processes.json"));
 config.cases={{"ok","0\n","0"},{"pending","2\n",""}};config.limits.timeMs=5000;runner.Start(config);auto waiting=GetTickCount64();while(runner.Snapshot().results.empty()&&GetTickCount64()-waiting<5000)Sleep(15);runner.Cancel();runner.Stop();CHECK(runner.Snapshot().score==0);config.limits.timeMs=300;
 std::string terminal;CHECK(utils::ReadFileU8((fs::u8path(runner.Snapshot().archive)/"run.json").wstring(),terminal));CHECK(nlohmann::json::parse(terminal).at("status")=="cancelled");
 utils::WriteFileU8(config.solution,"this is invalid C++;");state=run();CHECK(state.results.size()==1&&state.results[0].verdict=="CE");CHECK(utils::ReadFileU8((fs::u8path(state.archive)/"run.json").wstring(),terminal));CHECK(nlohmann::json::parse(terminal).at("status")=="error");
 utils::WriteFileU8(config.solution,"#include <iostream>\nint main(){int n;std::cin>>n;std::cout<<n+1;}");
 config.reference=(root/"reference.cpp").wstring();config.generator=(root/"generator.cpp").wstring();utils::WriteFileU8(config.reference,"#include <iostream>\nint main(){int n;std::cin>>n;std::cout<<n;}");utils::WriteFileU8(config.generator,"#include <iostream>\nint main(int n,char**v){std::cout<<v[1];}");
 config.stress=true;config.seed=17;config.rounds=3;state=run();CHECK(state.results.size()==1&&state.results[0].verdict=="WA");std::string input;CHECK(utils::ReadFileU8((fs::u8path(state.archive)/"case-1/input.txt").wstring(),input)&&input=="17");CHECK(fs::is_regular_file(fs::u8path(state.archive)/"generator.cpp.snapshot"));
 utils::WriteFileU8(config.generator,"int main(){return 3;}");state=run();CHECK(state.results.size()==1&&state.results[0].verdict=="GEN-RE");
 utils::WriteFileU8(config.generator,"int main(){for(;;){}}");config.limits.timeMs=5000;runner.Start(config);Sleep(100);runner.Cancel();auto start=GetTickCount64();runner.Stop();CHECK(GetTickCount64()-start<3000);
 fs::remove_all(root);std::cout<<"Workflow checks: "<<passed<<" passed\n";return 0;
}
