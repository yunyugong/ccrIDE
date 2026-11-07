#pragma once
#include <windows.h>
#include <atomic>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace execution {
enum class Reason { Exited, LaunchFailed, Cancelled, TimedOut, OutputLimit, MemoryLimit, ProcessLimit, IoError, CleanupFailed, QueueTimeout };
struct Limits {
    DWORD timeMs = 60000;          // Wall time; zero disables the deadline.
    size_t memoryBytes = 0;        // Aggregate Job commit; zero leaves it unlimited.
    size_t outputBytes = 16 * 1024 * 1024; // Combined stdout + stderr, always bounded.
};
enum class Lane { Foreground, Background, Service };
struct Task {
    std::string id;
    std::wstring directory;
};
struct TaskInfo {
    Task task;
    std::string label, state, lane, error;
    DWORD ownerPid=0, childPid=0, exitCode=0;
    ULONGLONG queuedMs=0, elapsedMs=0, createdTick=0;
    std::wstring cwd;
    bool active=false;
};
std::wstring TaskRoot();
bool CreateTask(Task& task,std::string& error,const std::wstring& root={});
std::vector<TaskInfo> ListTasks(const std::wstring& root={},size_t maximum=200);
bool CancelTask(const std::string& id);
const char* LaneText(Lane lane);
void DrawTaskWindow(bool* open);
struct Request {
    Task task;
    std::wstring taskRoot;
    std::string label="执行";
    Lane lane=Lane::Foreground;
    bool isolateWorkingDirectory=false;
    bool logOutput=true;
    DWORD queueTimeoutMs=30000;
    std::function<void(const TaskInfo&)> onState;
    std::function<void(DWORD)> onStarted;
    std::function<bool()> isCancelled;

    std::wstring exe, cwd;
    std::vector<std::wstring> args;
    std::map<std::wstring, std::wstring> environment; // Overrides inherited variables.
    Limits limits;
    bool noWindow = true;
    bool capture = true;
    std::string input;
    bool interactive = false;     // Otherwise input is followed by EOF.
    std::atomic<bool>* cancel = nullptr;
    // Called only on the executing worker, bounded chunks. Callbacks must not block.
    std::function<std::string()> pollInput;
    std::function<void(const std::string&, bool stderrChannel)> onOutput;
};
struct Result {
    Task task;ULONGLONG queuedMs=0;std::wstring cwd;

    bool started = false;
    Reason reason = Reason::LaunchFailed;
    DWORD exitCode = 0;
    ULONGLONG elapsedMs = 0;
    size_t outputBytes = 0, peakMemoryBytes = 0;
    std::string out, err, error;
};
Result Run(const Request& request); // Blocking worker API; never call from the UI thread.
std::wstring CommandLine(const std::wstring& exe, const std::vector<std::wstring>& args);
const char* ReasonText(Reason reason);
#ifdef MINICPP_EXECUTION_TESTING
void FailNextForTesting(const char* operation);
#endif
} // namespace execution
