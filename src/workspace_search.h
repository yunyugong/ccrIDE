#pragma once
#include <atomic>
#include <cstdint>
#include <condition_variable>
#include <memory>
#include <optional>
#include <mutex>
#include <thread>
#include <string>
#include <vector>
namespace workspace {
struct Hit {std::string file; int line=1; std::string text;};
struct SearchQuery {std::wstring directory; std::string needle; bool caseSensitive=false,wholeWord=false; std::vector<std::wstring> excluded;};
struct SearchSnapshot {std::vector<Hit> hits; size_t files=0; bool running=false,cancelled=false,truncated=false; std::string error;};
// One owner (UI), one worker. Starting/clearing always retires the preceding job.
class Search {
    struct Job {SearchQuery query;uint64_t generation;std::shared_ptr<std::atomic<bool>> cancel;};
    std::thread worker;std::mutex mutex;std::condition_variable wake;SearchSnapshot state;
    std::optional<Job> queued;std::shared_ptr<std::atomic<bool>> activeCancel;
    uint64_t stateVersion=0;
    uint64_t generation=0;bool stopping=false;
    void Run();void Execute(const Job& job);
public:
    ~Search(){Stop();}
    void Start(SearchQuery query);
    void Cancel();void Stop();void Clear();
    bool Poll(SearchSnapshot& out,uint64_t& seen){std::lock_guard<std::mutex> lock(mutex);if(seen==stateVersion)return false;out=state;seen=stateVersion;return true;}
    SearchSnapshot Snapshot(){std::lock_guard<std::mutex> lock(mutex);return state;}
};
}
