#include "background_index.h"
#include "utils.h"
#include <algorithm>
namespace background {
Index& Index::I(){static Index index;return index;}
void Index::Enable(){std::lock_guard<std::mutex> lock(mutex);if(enabled)return;enabled=true;stopping=false;worker=std::thread([this]{Run();});}
void Index::Stop(){
    {std::lock_guard<std::mutex> lock(mutex);stopping=true;queue.clear();if(working)active.cancel->store(true);}
    wake.notify_all();if(worker.joinable())worker.join();enabled=false;
}
void Index::Cancel(uint64_t uid){
    std::lock_guard<std::mutex> lock(mutex);
    if(working&&active.uid==uid)active.cancel->store(true);
    queue.erase(std::remove_if(queue.begin(),queue.end(),[&](const Job& job){return job.uid==uid;}),queue.end());
    results.erase(uid);
}
// Called under mutex. Each document has at most one queued revision; stale active
// revisions are cancelled and can never replace a newer result.
void Index::Queue(Job job){
    if(working&&active.uid==job.uid&&active.signature==job.signature&&active.compiler==job.compiler&&!active.cancel->load())return;
    for(auto& pending:queue)if(pending.uid==job.uid&&pending.signature==job.signature&&pending.compiler==job.compiler)return;
    if(working&&active.uid==job.uid)active.cancel->store(true);
    queue.erase(std::remove_if(queue.begin(),queue.end(),[&](const Job& p){return p.uid==job.uid;}),queue.end());
    if(queue.size()>=8)queue.pop_front();queue.push_back(std::move(job));wake.notify_one();
}
bool Index::Directories(const std::wstring& compiler,std::vector<std::wstring>& out){
    std::lock_guard<std::mutex> lock(mutex);auto it=directories.find(compiler);
    if(it!=directories.end()){out=it->second;return true;}
    Job job;job.compiler=compiler;Queue(std::move(job));return false;
}
std::shared_ptr<HeaderResult> Index::Headers(uint64_t uid,uint64_t signature,const std::string& text,const std::wstring& path,const std::wstring& compiler){
    std::lock_guard<std::mutex> lock(mutex);auto it=results.find(uid);
    if(it!=results.end()&&it->second->signature==signature){auto result=it->second;results.erase(it);return result;}
    if(working&&active.uid==uid&&active.signature==signature&&active.compiler==compiler&&!active.cancel->load())return {};
    for(const auto& pending:queue)if(pending.uid==uid&&pending.signature==signature&&pending.compiler==compiler)return {};
    results.erase(uid);Job job;job.uid=uid;job.signature=signature;job.text=text;job.path=path;job.compiler=compiler;Queue(std::move(job));return {};
}
void Index::Run(){
    for(;;){
        Job job;std::vector<std::wstring> dirs;bool cached=false;
        {std::unique_lock<std::mutex> lock(mutex);wake.wait(lock,[&]{return stopping||!queue.empty();});if(stopping)return;
            active=std::move(queue.front());queue.pop_front();working=true;job=active;
            auto it=directories.find(job.compiler);if(it!=directories.end()){dirs=it->second;cached=true;}}
        std::shared_ptr<HeaderResult> result;
        try {
            SymbolIndex index(job.compiler,job.cancel.get());
            if(cached)index.SetIncludeDirs(dirs);else dirs=index.IncludeDirs();
            if(job.uid&&!job.cancel->load()){
                result=std::make_shared<HeaderResult>();result->signature=job.signature;
                result->unit=index.BuildHeaderUnit(job.text,job.path);
                if(!job.cancel->load()){result->model.SetSystemSource(true);result->model.Rebuild(result->unit.text);}
            }
        }catch(...){result.reset();}
        {std::lock_guard<std::mutex> lock(mutex);
            if(!stopping&&!job.cancel->load()){
                // Bound compiler and inactive document caches.
                if(directories.size()>=4&&!directories.count(job.compiler))directories.erase(directories.begin());
                directories[job.compiler]=std::move(dirs);
                if(result){if(results.size()>=8)results.erase(results.begin());results[job.uid]=std::move(result);}
            }working=false;
        }
    }
}
}
