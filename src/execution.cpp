#include "execution.h"
#include "utils.h"
#include <algorithm>
#include <cwctype>
#include <memory>

namespace execution {
#ifdef MINICPP_EXECUTION_TESTING
static const char* failNext = nullptr;
void FailNextForTesting(const char* operation) { failNext = operation; }
#endif
namespace {
bool InjectedFailure(const char* operation) {
    (void)operation;
#ifdef MINICPP_EXECUTION_TESTING
    if (failNext && strcmp(failNext, operation) == 0) {
        failNext = nullptr; SetLastError(ERROR_ACCESS_DENIED); return true;
    }
#endif
    return false;
}
struct Handle {
    HANDLE value = nullptr;
    Handle() = default;
    explicit Handle(HANDLE h) : value(h) {}
    ~Handle() { reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    void reset(HANDLE h = nullptr) {
        if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value);
        value = h;
    }
    operator HANDLE() const { return value; }
    explicit operator bool() const { return value && value != INVALID_HANDLE_VALUE; }
};
std::string SystemError(const char* step) {
    DWORD code = GetLastError();
    wchar_t* message = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, code, 0, (wchar_t*)&message, 0, nullptr);
    std::string result = std::string(step) + " (Windows " + std::to_string(code) + ")";
    if (message) { result += ": " + utils::WToU8(message); LocalFree(message); }
    return result;
}
struct Attributes {
    std::vector<unsigned char> storage;
    LPPROC_THREAD_ATTRIBUTE_LIST list = nullptr;
    ~Attributes() { if (list) DeleteProcThreadAttributeList(list); }
    bool init(HANDLE* handles, size_t count) {
        SIZE_T bytes = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
        storage.resize(bytes);
        auto candidate = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
        if (!InitializeProcThreadAttributeList(candidate, 1, 0, &bytes)) return false;
        list = candidate;
        return UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles,
                                         sizeof(HANDLE) * count, nullptr, nullptr) != FALSE;
    }
};
bool Pipe(Handle& read, Handle& write, bool childReads) {
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    if (!CreatePipe(&read.value, &write.value, &security, 0)) return false;
    return SetHandleInformation(childReads ? write.value : read.value, HANDLE_FLAG_INHERIT, 0) != FALSE;
}
struct CaseLess {
    bool operator()(const std::wstring& a, const std::wstring& b) const { return _wcsicmp(a.c_str(), b.c_str()) < 0; }
};
std::vector<wchar_t> Environment(const std::map<std::wstring, std::wstring>& overrides) {
    std::map<std::wstring, std::wstring, CaseLess> vars;
    wchar_t* inherited = GetEnvironmentStringsW();
    if (inherited) {
        for (const wchar_t* p = inherited; *p; p += wcslen(p) + 1) {
            std::wstring entry(p);
            size_t split = entry.find(L'=', 1); // Preserve =C:=... drive variables.
            if (split != std::wstring::npos) vars[entry.substr(0, split)] = entry.substr(split + 1);
        }
        FreeEnvironmentStringsW(inherited);
    }
    for (const auto& pair : overrides) vars[pair.first] = pair.second;
    std::vector<wchar_t> block;
    for (const auto& pair : vars) {
        std::wstring entry = pair.first + L"=" + pair.second;
        block.insert(block.end(), entry.begin(), entry.end()); block.push_back(0);
    }
    if (block.empty()) block.push_back(0);
    block.push_back(0);
    return block;
}
}

std::wstring CommandLine(const std::wstring& exe, const std::vector<std::wstring>& args) {
    std::wstring line = utils::QuoteArg(exe);
    for (const auto& arg : args) line += L" " + utils::QuoteArg(arg);
    return line;
}
const char* ReasonText(Reason reason) {
    switch (reason) {
    case Reason::Exited: return "已退出";
    case Reason::LaunchFailed: return "启动失败";
    case Reason::Cancelled: return "已停止";
    case Reason::TimedOut: return "超时 (TLE)";
    case Reason::OutputLimit: return "输出超限 (OLE)";
    case Reason::MemoryLimit: return "内存超限 (MLE)";
    case Reason::ProcessLimit: return "进程数量超限";
    case Reason::IoError: return "输入输出失败";
    case Reason::CleanupFailed: return "进程清理失败";
    }
    return "未知状态";
}

Result Run(const Request& r) {
    Result result;
    const ULONGLONG start = GetTickCount64();
    auto fail = [&](const char* step) { result.error = SystemError(step); };
    if (r.cancel && r.cancel->load()) { result.reason = Reason::Cancelled; return result; }
    if (r.exe.empty() || !r.limits.outputBytes) { result.error = "可执行文件为空或输出上限为零"; return result; }
    if (r.exe.find(L'\0') != std::wstring::npos || r.cwd.find(L'\0') != std::wstring::npos) {
        result.error = "路径包含无效的空字符"; return result;
    }
    for (const auto& arg : r.args) if (arg.find(L'\0') != std::wstring::npos) {
        result.error = "程序参数包含无效的空字符"; return result;
    }
    std::wstring executable = r.exe;
    if (executable.find_first_of(L"/\\") == std::wstring::npos) {
        wchar_t found[32768];
        DWORD length = SearchPathW(nullptr, executable.c_str(), L".exe", 32768, found, nullptr);
        if (!length || length >= 32768) { fail("查找可执行文件失败"); return result; }
        executable.assign(found, length);
    }
    for (const auto& env : r.environment) {
        if (env.first.empty() || env.first.find_first_of(L"=\0", 0, 2) != std::wstring::npos ||
            env.second.find(L'\0') != std::wstring::npos) {
            result.error = "环境变量名称或内容无效"; return result;
        }
    }
    Handle job(InjectedFailure("job-create") ? nullptr : CreateJobObjectW(nullptr, nullptr));
    if (!job) { fail("创建进程组失败"); return result; }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    limits.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
    limits.BasicLimitInformation.ActiveProcessLimit = 128;
    if (r.limits.memoryBytes) {
        limits.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_JOB_MEMORY;
        limits.JobMemoryLimit = r.limits.memoryBytes;
    }
    if (InjectedFailure("job-configure") || !SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
        fail("设置进程组限制失败"); return result;
    }
    Handle port(CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1));
    if (!port) { fail("创建资源通知通道失败"); return result; }
    JOBOBJECT_ASSOCIATE_COMPLETION_PORT association{reinterpret_cast<void*>(1), port};
    if (!SetInformationJobObject(job, JobObjectAssociateCompletionPortInformation, &association, sizeof(association))) {
        fail("设置资源通知失败"); return result;
    }
    Handle outRead, outWrite, errRead, errWrite;
    if (!Pipe(outRead, outWrite, false) || !Pipe(errRead, errWrite, false)) {
        fail("创建输出管道失败"); return result;
    }
    // Overlapped stdin keeps a target which never reads input from blocking cancellation.
    static std::atomic<unsigned long> nextPipe{0};
    std::wstring name = L"\\\\.\\pipe\\MiniCPP.stdin." + std::to_wstring(GetCurrentProcessId()) + L"." +
                        std::to_wstring(++nextPipe);
    Handle inWrite(CreateNamedPipeW(name.c_str(), PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                   PIPE_TYPE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 4096, 4096, 0, nullptr));
    if (!inWrite) { fail("创建输入管道失败"); return result; }
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    Handle inRead(CreateFileW(name.c_str(), GENERIC_READ, 0, &security, OPEN_EXISTING, 0, nullptr));
    if (!inRead) { fail("连接输入管道失败"); return result; }
    Handle inputEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!inputEvent) { fail("创建输入事件失败"); return result; }
    OVERLAPPED writeOp{}; writeOp.hEvent = inputEvent;
    // Client already connected; accepting it must return ERROR_PIPE_CONNECTED.
    if (!ConnectNamedPipe(inWrite, &writeOp) && GetLastError() != ERROR_PIPE_CONNECTED) {
        fail("接受输入管道连接失败"); return result;
    }
    HANDLE inherited[] = {inRead, outWrite, errWrite};
    Attributes attributes;
    if (!attributes.init(inherited, 3)) { fail("限制继承句柄失败"); return result; }
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = inRead;
    startup.StartupInfo.hStdOutput = outWrite;
    startup.StartupInfo.hStdError = errWrite;
    startup.lpAttributeList = attributes.list;
    std::wstring command = CommandLine(r.exe, r.args);
    std::vector<wchar_t> env;
    if (!r.environment.empty()) env = Environment(r.environment);
    PROCESS_INFORMATION info{};
    DWORD flags = CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT;
    if (r.noWindow) flags |= CREATE_NO_WINDOW;
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE, flags,
                        env.empty() ? nullptr : env.data(), r.cwd.empty() ? nullptr : r.cwd.c_str(),
                        &startup.StartupInfo, &info)) {
        fail("创建进程失败"); return result;
    }
    Handle process(info.hProcess), thread(info.hThread);
    auto abortLaunch = [&](bool assigned) {
        BOOL killed = assigned ? TerminateJobObject(job, 1) : TerminateProcess(process, 1);
        if (!killed || WaitForSingleObject(process, 3000) != WAIT_OBJECT_0) {
            result.reason = Reason::CleanupFailed;
            result.error += " 未启动的目标进程清理失败";
        }
    };
    if (InjectedFailure("job-assign") || !AssignProcessToJobObject(job, process)) {
        fail("进程无法加入受控进程组，已取消启动");
        abortLaunch(false);
        return result;
    }
    if (InjectedFailure("resume") || ResumeThread(thread) == DWORD(-1)) {
        fail("恢复进程失败"); abortLaunch(true); return result;
    }
    result.started = true; result.reason = Reason::Exited;
    thread.reset(); inRead.reset(); outWrite.reset(); errWrite.reset();
    std::string pendingInput = r.input;
    size_t inputPos = 0;
    bool writing = false;
    std::map<DWORD, std::unique_ptr<Handle>> tracked;
    auto track = [&](DWORD pid) {
        if (!tracked.count(pid)) {
            HANDLE child = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (child) {
                BOOL belongs = FALSE;
                if (IsProcessInJob(child, job, &belongs) && belongs) tracked.emplace(pid, std::make_unique<Handle>(child));
                else CloseHandle(child);
            }
        }
    };
    auto stopJob = [&]() {
        // Hold process handles before termination: Job ActiveProcesses can reach zero
        // slightly before individual process handles become signaled.
        std::vector<ULONG_PTR> storage(256);
        auto* ids = reinterpret_cast<JOBOBJECT_BASIC_PROCESS_ID_LIST*>(storage.data());
        if (QueryInformationJobObject(job, JobObjectBasicProcessIdList, ids,
                                      (DWORD)(storage.size() * sizeof(ULONG_PTR)), nullptr))
            for (DWORD i = 0; i < ids->NumberOfProcessIdsInList; ++i) track((DWORD)ids->ProcessIdList[i]);
        return TerminateJobObject(job, 1) != FALSE;
    };
    auto notifications = [&]() {
        for (auto it = tracked.begin(); it != tracked.end();)
            if (WaitForSingleObject(*it->second, 0) == WAIT_OBJECT_0) it = tracked.erase(it); else ++it;
        DWORD message; ULONG_PTR key; OVERLAPPED* data;
        for (int n = 0; n < 512 && GetQueuedCompletionStatus(port, &message, &key, &data, 0); ++n) {
            if (message == JOB_OBJECT_MSG_NEW_PROCESS) track((DWORD)reinterpret_cast<ULONG_PTR>(data));
            if (message == JOB_OBJECT_MSG_JOB_MEMORY_LIMIT || message == JOB_OBJECT_MSG_PROCESS_MEMORY_LIMIT)
                if (result.reason == Reason::Exited) result.reason = Reason::MemoryLimit;
            if (message == JOB_OBJECT_MSG_ACTIVE_PROCESS_LIMIT && result.reason == Reason::Exited)
                result.reason = Reason::ProcessLimit;
        }
    };
    auto drain = [&](HANDLE pipe, bool isErr) {
        // Budget is per channel per iteration; cancellation is checked between reads.
        for (size_t budget = 0; budget < 65536 && result.reason == Reason::Exited;) {
            if (r.cancel && r.cancel->load()) { result.reason = Reason::Cancelled; break; }
            DWORD available = 0;
            if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) {
                if (GetLastError() != ERROR_BROKEN_PIPE) { result.reason = Reason::IoError; fail("读取输出状态失败"); }
                break;
            }
            if (!available) break;
            char buffer[8192]; DWORD got = 0;
            if (!ReadFile(pipe, buffer, (DWORD)(std::min)(sizeof(buffer), (size_t)available), &got, nullptr)) {
                if (GetLastError() != ERROR_BROKEN_PIPE) { result.reason = Reason::IoError; fail("读取输出失败"); }
                break;
            }
            if (!got) break;
            size_t accepted = (std::min)((size_t)got, r.limits.outputBytes - result.outputBytes);
            std::string text(buffer, accepted);
            result.outputBytes += accepted; budget += got;
            if (r.capture) (isErr ? result.err : result.out).append(text);
            if (r.onOutput && accepted) r.onOutput(text, isErr);
            if (accepted < got) { result.reason = Reason::OutputLimit; break; }
        }
    };
    auto inputTick = [&]() {
        if (!inWrite) return;
        DWORD sent = 0;
        if (writing) {
            if (!GetOverlappedResult(inWrite, &writeOp, &sent, FALSE)) {
                DWORD error = GetLastError();
                if (error == ERROR_IO_INCOMPLETE) return;
                writing = false;
                if (error != ERROR_BROKEN_PIPE && error != ERROR_NO_DATA) { result.reason = Reason::IoError; fail("发送输入失败"); }
                inWrite.reset(); return;
            }
            writing = false; inputPos += sent;
        }
        if (inputPos == pendingInput.size()) {
            pendingInput.clear(); inputPos = 0;
            if (r.interactive && r.pollInput) pendingInput = r.pollInput();
            if (pendingInput.empty()) { if (!r.interactive) inWrite.reset(); return; }
        }
        ResetEvent(inputEvent);
        writeOp = OVERLAPPED{}; writeOp.hEvent = inputEvent;
        DWORD bytes = (DWORD)(std::min)(size_t(8192), pendingInput.size() - inputPos);
        if (WriteFile(inWrite, pendingInput.data() + inputPos, bytes, &sent, &writeOp)) inputPos += sent;
        else if (GetLastError() == ERROR_IO_PENDING) writing = true;
        else {
            DWORD error = GetLastError();
            if (error != ERROR_BROKEN_PIPE && error != ERROR_NO_DATA) { result.reason = Reason::IoError; fail("发送输入失败"); }
            inWrite.reset();
        }
    };
    bool parentExited = false;
    for (;;) {
        notifications();
        if (r.cancel && r.cancel->load() && result.reason == Reason::Exited) result.reason = Reason::Cancelled;
        if (result.reason == Reason::Exited && r.limits.timeMs && GetTickCount64() - start >= r.limits.timeMs)
            result.reason = Reason::TimedOut;
        if (result.reason != Reason::Exited) break;
        drain(outRead, false); drain(errRead, true);
        if (result.reason != Reason::Exited) break;
        if (parentExited) {
            DWORD outAvail = 0, errAvail = 0;
            PeekNamedPipe(outRead, nullptr, 0, nullptr, &outAvail, nullptr);
            PeekNamedPipe(errRead, nullptr, 0, nullptr, &errAvail, nullptr);
            if (!outAvail && !errAvail) break;
            continue;
        }
        inputTick();
        DWORD wait = WaitForSingleObject(process, 10);
        if (wait == WAIT_FAILED) { result.reason = Reason::IoError; fail("等待进程失败"); break; }
        if (wait == WAIT_OBJECT_0) {
            parentExited = true;
            // The main process may exit leaving descendants holding pipes open.
            if (!stopJob()) { result.reason = Reason::CleanupFailed; fail("清理后代进程失败"); break; }
        }
    }
    // Stop the entire job on every path, even after a normal main-process exit.
    if (!parentExited && !stopJob()) { result.reason = Reason::CleanupFailed; fail("终止进程组失败"); }
    if (writing) { DWORD sent = 0; CancelIoEx(inWrite, &writeOp); GetOverlappedResult(inWrite, &writeOp, &sent, TRUE); }
    inWrite.reset();
    if (WaitForSingleObject(process, 3000) != WAIT_OBJECT_0) { result.reason = Reason::CleanupFailed; result.error += " 等待主进程退出失败"; }
    const ULONGLONG cleanupStart = GetTickCount64();
    for (const auto& child : tracked) {
        ULONGLONG elapsed = GetTickCount64() - cleanupStart;
        DWORD remaining = elapsed >= 3000 ? 0 : (DWORD)(3000 - elapsed);
        if (WaitForSingleObject(*child.second, remaining) != WAIT_OBJECT_0) {
            result.reason = Reason::CleanupFailed; result.error += " 等待后代进程退出失败";
        }
    }
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
    for (;;) {
        if (!QueryInformationJobObject(job, JobObjectBasicAccountingInformation, &accounting, sizeof(accounting), nullptr)) {
            result.reason = Reason::CleanupFailed; fail("查询进程组清理状态失败"); break;
        }
        if (!accounting.ActiveProcesses) break;
        if (GetTickCount64() - cleanupStart >= 3000) { result.reason = Reason::CleanupFailed; result.error += " 进程组仍有未退出的进程"; break; }
        Sleep(5);
    }
    notifications();
    if (QueryInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits), nullptr))
        result.peakMemoryBytes = limits.PeakJobMemoryUsed;
    if (!GetExitCodeProcess(process, &result.exitCode)) { result.reason = Reason::CleanupFailed; fail("读取退出码失败"); }
    result.elapsedMs = GetTickCount64() - start;
    return result;
}
} // namespace execution
