#pragma once
#include "execution.h"
#include <string>
#include <vector>
#include <thread>
#include <mutex>
namespace contest {
enum class Compare {Exact,Tokens};
struct Case {std::string name,input,expected,group="default";bool enabled=true;};
struct Group {std::string name="default";double points=100;std::vector<std::string> requires;};
struct Config {std::wstring compiler,solution,reference,generator,archive;std::vector<std::wstring> flags;std::vector<Case> cases;std::vector<Group> groups;Compare compare=Compare::Tokens;execution::Limits limits{2000,256*1024*1024,16*1024*1024};int rounds=100;unsigned long long seed=1;bool stress=false;bool isolatedWorkingDirectory=true;};
struct Result {std::string name,verdict,output,error,difference,group;unsigned long long elapsed=0;size_t peak=0;unsigned long exitCode=0;std::string termination;};
struct State {bool running=false;std::string status,archive;std::vector<Result> results;double score=0;};
bool Equal(const std::string& expected,const std::string& actual,Compare mode,std::string& difference);
double Score(const std::vector<Group>& groups,const std::vector<Result>& results);
class Runner {
 std::thread worker;std::atomic<bool> cancel{false};std::mutex mutex;State state;
 public:~Runner(){Stop();}void Start(Config config);void Stop();void Cancel(){cancel=true;}State Snapshot();
};
void Draw(bool* open);void Shutdown();
}
