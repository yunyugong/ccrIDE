#include "contest.h"
#include "recovery.h"
#include "utils.h"
#include <filesystem>
#include <iostream>
#include <fstream>
namespace fs=std::filesystem;
#define CHECK(x) do{if(!(x)){std::cerr<<"FAIL "<<__LINE__<<": "<<#x<<"\n";return 1;}++passed;}while(0)
int wmain(int argc,wchar_t** argv){
 int passed=0;if(argc!=2)return 2;auto root=fs::temp_directory_path()/(L"ccride-workflow-中文 "+std::to_wstring(GetCurrentProcessId()));fs::create_directories(root);
 recovery::Snapshot snapshot;snapshot.active=1;snapshot.tabs.push_back({"saved","original.cpp","int changed;","int old;",5,2,true});snapshot.tabs.push_back({"untitled","","","",0,0,true});std::string error;
 CHECK(recovery::Save((root/"recovery").wstring(),snapshot,error));recovery::Snapshot restored;CHECK(recovery::Load((root/"recovery").wstring(),restored,error));CHECK(restored.tabs.size()==2&&restored.tabs[1].text.empty()&&restored.active==1&&restored.tabs[0].anchor==2);
 auto next=snapshot;next.tabs[0].text="second";CHECK(recovery::Save((root/"recovery").wstring(),next,error));utils::WriteFileU8((root/"recovery/workspace.json").wstring(),"{broken");CHECK(recovery::Load((root/"recovery").wstring(),restored,error));CHECK(restored.tabs[0].text=="int changed;"&&!error.empty());
 utils::WriteFileU8((root/"not-a-directory").wstring(),"x");CHECK(!recovery::Save((root/"not-a-directory").wstring(),snapshot,error));
 std::string diff;CHECK(contest::Equal("a b\n","a\tb ",contest::Compare::Tokens,diff));CHECK(!contest::Equal("a b\n","a\tb ",contest::Compare::Exact,diff));CHECK(!contest::Equal("1.0","1",contest::Compare::Tokens,diff));
 std::vector<contest::Group> groups{{"a",30,{}},{"b",70,{"a"}}};std::vector<contest::Result> results{{"1","WA","","","","a"},{"2","AC","","","","b"}};CHECK(contest::Score(groups,results)==0);results[0].verdict="AC";CHECK(contest::Score(groups,results)==100);groups[0].requires={"b"};CHECK(contest::Score(groups,results)==0);
 contest::Config config;config.compiler=argv[1];config.archive=(root/"runs").wstring();config.solution=(root/"solution.cpp").wstring();config.flags={L"-std=c++17",L"-O0"};config.limits={300,64*1024*1024,65536};config.groups={{"default",100,{}}};
 utils::WriteFileU8(config.solution,"#include <iostream>\nint main(){int n;std::cin>>n;if(n<0)return 7;if(n==4){auto p=new char[256*1024*1024];for(int i=0;i<256*1024*1024;++i)p[i]=1;}if(n==2)for(;;){}if(n==3)for(;;)std::cout<<\"xxxxxxxxxxxxxxxx\";std::cout<<n;}\n");
 config.cases={{"ok","0\n","0"},{"wrong","1\n","9"},{"runtime","-1\n",""},{"timeout","2\n",""},{"flood","3\n",""},{"memory","4\n",""}};
 contest::Runner runner;auto run=[&](){runner.Start(config);auto start=GetTickCount64();while(runner.Snapshot().running&&GetTickCount64()-start<30000)Sleep(15);auto state=runner.Snapshot();runner.Stop();return state;};
 auto state=run();CHECK(state.results.size()==6);const char* expected[]={"AC","WA","RE","TLE","OLE","MLE"};for(int i=0;i<6;++i)CHECK(state.results[i].verdict==expected[i]);CHECK(fs::is_regular_file(fs::u8path(state.archive)/"case-2/input.txt"));CHECK(fs::is_regular_file(fs::u8path(state.archive)/"run.json"));
 config.cases={{"ok","0\n","0"},{"pending","2\n",""}};config.limits.timeMs=5000;runner.Start(config);auto waiting=GetTickCount64();while(runner.Snapshot().results.empty()&&GetTickCount64()-waiting<5000)Sleep(15);runner.Cancel();runner.Stop();CHECK(runner.Snapshot().score==0);config.limits.timeMs=300;
 utils::WriteFileU8(config.solution,"this is invalid C++;");state=run();CHECK(state.results.size()==1&&state.results[0].verdict=="CE");
 utils::WriteFileU8(config.solution,"#include <iostream>\nint main(){int n;std::cin>>n;std::cout<<n+1;}");
 config.reference=(root/"reference.cpp").wstring();config.generator=(root/"generator.cpp").wstring();utils::WriteFileU8(config.reference,"#include <iostream>\nint main(){int n;std::cin>>n;std::cout<<n;}");utils::WriteFileU8(config.generator,"#include <iostream>\nint main(int n,char**v){std::cout<<v[1];}");
 config.stress=true;config.seed=17;config.rounds=3;state=run();CHECK(state.results.size()==1&&state.results[0].verdict=="WA");std::string input;CHECK(utils::ReadFileU8((fs::u8path(state.archive)/"case-1/input.txt").wstring(),input)&&input=="17");CHECK(fs::is_regular_file(fs::u8path(state.archive)/"generator.cpp.snapshot"));
 utils::WriteFileU8(config.generator,"int main(){return 3;}");state=run();CHECK(state.results.size()==1&&state.results[0].verdict=="GEN-RE");
 utils::WriteFileU8(config.generator,"int main(){for(;;){}}");config.limits.timeMs=5000;runner.Start(config);Sleep(100);runner.Cancel();auto start=GetTickCount64();runner.Stop();CHECK(GetTickCount64()-start<3000);
 fs::remove_all(root);std::cout<<"Workflow checks: "<<passed<<" passed\n";return 0;
}
