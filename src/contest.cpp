#include "contest.h"
#include "utils.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <map>
#include <set>
#include <cmath>
#include <nlohmann/json.hpp>
namespace contest {
namespace fs=std::filesystem;using nlohmann::json;
bool Equal(const std::string& expected,const std::string& actual,Compare mode,std::string& difference){
 std::string a=expected,b=actual;
 if(mode==Compare::Tokens){auto tokens=[](const std::string& s){std::istringstream in(s);std::string t,out;while(in>>t){out+=t;out+='\n';}return out;};a=tokens(a);b=tokens(b);}
 size_t at=0;while(at<a.size()&&at<b.size()&&a[at]==b[at])++at;
 if(at==a.size()&&at==b.size()){difference.clear();return true;}
 difference="首个差异 byte "+std::to_string(at)+": expected "+a.substr(at,120)+" / actual "+b.substr(at,120);return false;
}
double Score(const std::vector<Group>& groups,const std::vector<Result>& results){
 std::map<std::string,const Group*> definitions;for(auto& g:groups){if(definitions.count(g.name)||g.points<0||!std::isfinite(g.points))return 0;definitions[g.name]=&g;}
 std::map<std::string,int> marks;std::function<bool(const std::string&)> pass=[&](const std::string& name){if(!definitions.count(name))return false;if(marks[name])return marks[name]==2;marks[name]=1;bool seen=false,ok=true;for(auto& r:results)if(r.group==name){seen=true;ok&=r.verdict=="AC";}for(auto& d:definitions[name]->requires)ok&=pass(d);marks[name]=seen&&ok?2:3;return seen&&ok;};
 double score=0;for(auto& g:groups)if(pass(g.name))score+=g.points;return score;
}
static std::string Verdict(const execution::Result& r){
 if(!r.started)return "启动失败";switch(r.reason){case execution::Reason::TimedOut:return "TLE";case execution::Reason::MemoryLimit:return "MLE";case execution::Reason::OutputLimit:return "OLE";case execution::Reason::Cancelled:return "取消";case execution::Reason::Exited:return r.exitCode?"RE":"AC";default:return "执行失败";}
}
State Runner::Snapshot(){std::lock_guard<std::mutex> lock(mutex);return state;}
void Runner::Stop(){cancel=true;if(worker.joinable())worker.join();}
void Runner::Start(Config config){
 Stop();cancel=false;{std::lock_guard<std::mutex> lock(mutex);state={};state.running=true;state.status="正在编译";}
 worker=std::thread([this,config](){
  fs::path root;std::vector<Result> results;size_t completedCases=0;
  auto score=[&](){if(config.stress)return 0.0;std::vector<Result> checks;for(auto& result:results)checks.push_back({result.name,result.verdict,"","","",result.group});size_t index=0;for(auto& test:config.cases)if(test.enabled){if(index++>=completedCases)checks.push_back({test.name,"未运行","","","",test.group});}return Score(config.groups,checks);};
  auto publish=[&](std::string status){std::lock_guard<std::mutex> lock(mutex);state.status=std::move(status);state.results.assign(results.begin()+(results.size()>1000?results.size()-1000:0),results.end());state.score=score();if(!root.empty())state.archive=utils::WToU8(root.wstring());};
  try{
   if(config.cases.size()>1000 || config.groups.size()>1000 || config.rounds<1 || config.rounds>10000 || config.seed>~0ull-(unsigned)config.rounds)throw std::runtime_error("每批最多 1000 样例 / 10000 轮对拍");
   root=fs::path(config.archive)/(L"run-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));if(!fs::create_directories(root))throw std::runtime_error("归档目录已存在");
   json metadata={{"schema",1},{"seed",config.seed},{"rounds",config.rounds},{"stress",config.stress},{"comparison",config.compare==Compare::Exact?"exact":"tokens"},{"timeMs",config.limits.timeMs},{"memoryBytes",config.limits.memoryBytes},{"outputBytes",config.limits.outputBytes},{"compiler",utils::WToU8(config.compiler)},{"flags",json::array()},{"programs",json::array()}};
   for(auto& f:config.flags)metadata["flags"].push_back(utils::WToU8(f));
   size_t archivedBytes=0;
   auto write=[&](const fs::path& file,const std::string& text){if(text.size()>512ull*1024*1024-archivedBytes)throw std::runtime_error("归档达到 512 MiB 上限，已停止");archivedBytes+=text.size();if(!utils::WriteFileU8(file.wstring(),text))throw std::runtime_error("归档写入失败");};
   auto compile=[&](const std::wstring& source,const std::string& role)->fs::path{
    std::string code;if(!utils::ReadFileU8(source,code))throw std::runtime_error("无法读取 "+role+" 源码");write(root/(role+".cpp.snapshot"),code);
    metadata["programs"].push_back({{"role",role},{"source",utils::WToU8(source)},{"hash",std::hash<std::string>{}(code)}});
    write(root/"run.json",metadata.dump(2));
    auto exe=root/(role+".exe");execution::Request q;q.exe=config.compiler;q.cwd=fs::path(source).parent_path().wstring();q.args=config.flags;q.args.insert(q.args.end(),{source,L"-o",exe.wstring()});q.cancel=&cancel;q.limits={120000,0,4*1024*1024};
    auto c=execution::Run(q);write(root/(role+"-compile.log"),c.out+c.err);
    if(c.reason!=execution::Reason::Exited||c.exitCode||!c.started){results.push_back({role,c.reason==execution::Reason::Cancelled?"取消":!c.started?"编译器启动失败":c.reason==execution::Reason::TimedOut?"编译超时":"CE","",c.err});throw std::runtime_error(role+" 编译未成功");}return exe;
   };
   auto sol=compile(config.solution,"solution");fs::path ref,gen;if(config.stress){ref=compile(config.reference,"reference");gen=compile(config.generator,"generator");}
   auto run=[&](const fs::path& exe,const std::string& input,std::vector<std::wstring> args=std::vector<std::wstring>{}){execution::Request q;q.exe=exe.wstring();q.cwd=fs::path(config.solution).parent_path().wstring();q.input=input;q.args=std::move(args);q.cancel=&cancel;q.limits=config.limits;return execution::Run(q);};
   int index=0;
   auto archiveCase=[&](const Result& result,const std::string& input,const std::string& expected,const std::string& referenceError="",const std::string& generatorError=""){
    auto dir=root/("case-"+std::to_string(++index));fs::create_directories(dir);write(dir/"input.txt",input);write(dir/"expected.txt",expected);write(dir/"actual.txt",result.output);write(dir/"stderr.txt",result.error);write(dir/"reference-stderr.txt",referenceError);write(dir/"generator-stderr.txt",generatorError);
    write(dir/"result.json",json{{"name",result.name},{"group",result.group},{"verdict",result.verdict},{"elapsedMs",result.elapsed},{"peakMemoryBytes",result.peak},{"difference",result.difference},{"seed",config.seed+index-1}}.dump(2));auto display=result;display.output.resize(std::min<size_t>(display.output.size(),4096));display.error.resize(std::min<size_t>(display.error.size(),4096));results.push_back(std::move(display));publish("已完成 "+std::to_string(index)+" 项");
   };
   if(!config.stress){
    for(const auto& test:config.cases)if(test.enabled){if(cancel)break;auto process=run(sol,test.input);Result result{test.name,Verdict(process),process.out,process.err,"",test.group,process.elapsedMs,process.peakMemoryBytes};if(result.verdict=="AC"&&!Equal(test.expected,process.out,config.compare,result.difference))result.verdict="WA";++completedCases;archiveCase(result,test.input,test.expected);}
   }else for(int round=0;round<config.rounds&&!cancel;++round){
    auto seed=config.seed+round;auto generated=run(gen,"",{std::to_wstring(seed)});
    if(Verdict(generated)!="AC"){archiveCase({"seed "+std::to_string(seed),"GEN-"+Verdict(generated),generated.out,generated.err},generated.out,"","",generated.err);break;}
    auto reference=run(ref,generated.out);
    if(Verdict(reference)!="AC"){archiveCase({"seed "+std::to_string(seed),"REF-"+Verdict(reference),reference.out,reference.err},generated.out,reference.out,reference.err,generated.err);break;}
    auto process=run(sol,generated.out);Result result{"seed "+std::to_string(seed),Verdict(process),process.out,process.err,"","",process.elapsedMs,process.peakMemoryBytes};if(result.verdict=="AC"&&!Equal(reference.out,process.out,config.compare,result.difference))result.verdict="WA";
    archiveCase(result,generated.out,reference.out,reference.err,generated.err);if(result.verdict!="AC")break;
   }
   json report=json::array();for(auto& result:results)report.push_back({{"name",result.name},{"group",result.group},{"verdict",result.verdict},{"elapsedMs",result.elapsed}});
   metadata["results"]=report;metadata["score"]=score();metadata["cancelled"]=cancel.load();write(root/"run.json",metadata.dump(2));publish(cancel?"已取消，结果已归档":"完成，结果已归档");
  }catch(const std::exception& e){if(!root.empty())utils::WriteFileU8((root/"error.txt").wstring(),e.what());publish(e.what());}
  std::lock_guard<std::mutex> lock(mutex);state.running=false;
 });
}
}
