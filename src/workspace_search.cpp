#include "workspace_search.h"
#include "utils.h"
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <cctype>
namespace workspace {
namespace fs=std::filesystem;
static std::string Lower(std::string s){for(auto& c:s)c=(char)tolower((unsigned char)c);return s;}
void Search::Start(SearchQuery query){
    std::lock_guard<std::mutex> lock(mutex);
    if(activeCancel)activeCancel->store(true);
    if(queued)queued->cancel->store(true);
    state={};state.running=true;++stateVersion;
    queued=Job{std::move(query),++generation,std::make_shared<std::atomic<bool>>(false)};
    if(!worker.joinable()){stopping=false;worker=std::thread([this]{Run();});}
    wake.notify_one();
}
void Search::Cancel(){std::lock_guard<std::mutex> lock(mutex);if(activeCancel)activeCancel->store(true);if(queued)queued->cancel->store(true);}
void Search::Stop(){
    {std::lock_guard<std::mutex> lock(mutex);stopping=true;if(activeCancel)activeCancel->store(true);queued.reset();}
    wake.notify_one();if(worker.joinable())worker.join();
}
void Search::Clear(){std::lock_guard<std::mutex> lock(mutex);if(activeCancel)activeCancel->store(true);queued.reset();++generation;state={};++stateVersion;}
void Search::Run(){
    for(;;){std::optional<Job> job;
        {std::unique_lock<std::mutex> lock(mutex);wake.wait(lock,[&]{return stopping||queued.has_value();});if(stopping)return;job=std::move(queued);queued.reset();activeCancel=job->cancel;}
        Execute(*job);
    }
}
void Search::Execute(const Job& job){
        const auto& query=job.query;auto& cancel=*job.cancel;
        SearchSnapshot result;result.running=true;
        auto publish=[&]{std::lock_guard<std::mutex> lock(mutex);if(!stopping&&job.generation==generation){state=result;++stateVersion;}};
        try {
            fs::path root(query.directory);std::error_code ec;
            if(query.needle.empty())throw std::runtime_error("Empty search text");
            if(!fs::is_directory(root,ec))throw std::runtime_error("Search directory does not exist");
            std::string needle=query.caseSensitive?query.needle:Lower(query.needle);
            auto boundary=[](unsigned char c){return isalnum(c)||c=='_'||c>=128;};
            auto matches=[&](const std::string& text){
                auto probe=query.caseSensitive?text:Lower(text);size_t p=0;
                while((p=probe.find(needle,p))!=std::string::npos){size_t end=p+needle.size();
                    if(!query.wholeWord||((p==0||!boundary(probe[p-1]))&&(end==probe.size()||!boundary(probe[end]))))return true;
                    p+=needle.size();}return false;};
            auto skip=[&](const fs::path& p){
                auto n=Lower(utils::WToU8(p.filename().wstring()));
                if(n.empty()||n[0]=='.'||n=="build"||n=="node_modules"||n=="bin"||n=="obj"||n=="backup")return true;
                std::error_code pathError;auto normalized=fs::absolute(p,pathError).lexically_normal().wstring();
                for(auto& item:query.excluded)if(!item.empty()){
                    auto excluded=fs::absolute(fs::path(item),pathError).lexically_normal().wstring();
                    if(_wcsicmp(normalized.c_str(),excluded.c_str())==0)return true;
                }
                return false;};
            const std::string extensions="|.cpp|.cc|.cxx|.c|.h|.hpp|.hh|.txt|.md|.json|.ini|.cfg|.log|.xml|.html|.css|.js|.ts|.py|.csv|.cmake|.mk|.bat|.ps1|";
            fs::recursive_directory_iterator it,end;
            DWORD rootAttributes=GetFileAttributesW(root.c_str());
            if(!skip(root)&&rootAttributes!=INVALID_FILE_ATTRIBUTES&&!(rootAttributes&FILE_ATTRIBUTE_REPARSE_POINT))
                it=fs::recursive_directory_iterator(root,fs::directory_options::skip_permission_denied,ec);
            ULONGLONG published=GetTickCount64();
            while(it!=end&&!cancel.load()&&result.hits.size()<4000){
                auto p=it->path();DWORD attrs=GetFileAttributesW(p.c_str());
                bool directory=it->is_directory(ec);ec.clear();
                // Never follow junctions/symlinks, including cycles and external trees.
                bool ignored=attrs==INVALID_FILE_ATTRIBUTES||(attrs&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_HIDDEN));
                if(directory){if(ignored||skip(p))it.disable_recursion_pending();}
                else if(!ignored && extensions.find("|"+Lower(p.extension().u8string())+"|")!=std::string::npos){
                    auto bytes=fs::file_size(p,ec);
                    if(!ec&&bytes>0&&bytes<=(2u<<20)){
                        // Bounded read even if the file grows after file_size().
                        std::ifstream file(p,std::ios::binary);std::string content(size_t(bytes)+1,'\0');
                        file.read(content.data(),std::streamsize(content.size()));content.resize(size_t(file.gcount()));
                        ++result.files;
                        if(content.size()<=(2u<<20)&&content.find('\0')==std::string::npos){
                            if(!utils::IsValidUtf8(content))content=utils::GbkToUtf8(content);
                            size_t start=0;int line=1;
                            while(start<content.size()&&!cancel.load()&&result.hits.size()<4000){
                                auto nl=content.find('\n',start);auto length=(nl==std::string::npos?content.size():nl)-start;
                                std::string text=content.substr(start,length);
                                if(matches(text)){
                                    size_t cut=std::min<size_t>(text.size(),1024);while(cut<text.size()&&cut>0&&((unsigned char)text[cut]&0xc0)==0x80)--cut;
                                    text.resize(cut);result.hits.push_back({p.u8string(),line,std::move(text)});
                                }
                                if(nl==std::string::npos)break;start=nl+1;++line;
                            }
                        }
                    }ec.clear();
                }
                it.increment(ec);if(ec)ec.clear();
                if(GetTickCount64()-published>=100){publish();published=GetTickCount64();}
            }
            result.truncated=result.hits.size()>=4000;
        }catch(const std::exception& e){result.error=e.what();}
        result.running=false;result.cancelled=cancel.load();publish();
    }
}
