#include "execution.h"
#include "utils.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>

static int failures = 0, passed = 0;
#define CHECK(condition) do { if (condition) ++passed; else { ++failures; std::cout << "FAIL line " << __LINE__ << ": " #condition "\n"; } } while (0)
int main() {
    namespace fs = std::filesystem;
    const fs::path fixture = fs::path(utils::GetExeDir()) / L"execution_fixture.exe";
    const fs::path dir = fs::temp_directory_path() / (L"MiniCPP execution 测试 " + std::to_wstring(GetCurrentProcessId()));
    fs::create_directories(dir);
    execution::Request request; request.exe = fixture.wstring(); request.cwd = dir.wstring();
    request.limits.timeMs = 2000; request.limits.outputBytes = 1024 * 1024;
    request.args = {L"echo"}; request.input = std::string(128 * 1024, 'a') + "\nUTF-8 中文\n";
    auto result = execution::Run(request);
    CHECK(result.started); CHECK(result.reason == execution::Reason::Exited); CHECK(result.exitCode == 0);
    CHECK(result.out == request.input); CHECK(result.err == "stderr marker");
    CHECK(result.outputBytes == result.out.size() + result.err.size());
    CHECK(result.peakMemoryBytes > 0);
    auto concurrentRequest = request;
    concurrentRequest.input = "independent input\n";
    execution::Result concurrentResult;
    std::thread concurrent([&]() { concurrentResult = execution::Run(concurrentRequest); });
    result = execution::Run(request); concurrent.join();
    CHECK(result.out == request.input && result.reason == execution::Reason::Exited);
    CHECK(concurrentResult.out == concurrentRequest.input && concurrentResult.reason == execution::Reason::Exited);
    request.input = std::string(1000, 'b'); request.limits.outputBytes = 1005;
    result = execution::Run(request);
    CHECK(result.reason == execution::Reason::OutputLimit && result.outputBytes == 1005);
    request.limits.outputBytes = 1013;
    result = execution::Run(request);
    CHECK(result.reason == execution::Reason::Exited && result.outputBytes == 1013);
    request.limits.outputBytes = 1024 * 1024;

    request.input.clear(); request.exe = (dir / L"not-found.exe").wstring();
    result = execution::Run(request);
    CHECK(!result.started); CHECK(result.reason == execution::Reason::LaunchFailed); CHECK(!result.error.empty());
    request.exe = fixture.wstring(); request.cwd = (dir / "missing").wstring();
    result = execution::Run(request); CHECK(!result.started); CHECK(!result.error.empty());
    request.cwd = dir.wstring();
    for (const char* fault : {"job-create", "job-configure", "job-assign", "resume"}) {
        request.args = {L"touch"};
        execution::FailNextForTesting(fault);
        result = execution::Run(request);
        CHECK(!result.started); CHECK(result.reason == execution::Reason::LaunchFailed);
        CHECK(!result.error.empty()); CHECK(!fs::exists(dir / "started-marker"));
    }

    std::ofstream(dir / L"cwd-marker") << "marker";
    const auto copiedFixture = dir / L"测试 helper.exe";
    fs::copy_file(fixture, copiedFixture);
    request.exe = copiedFixture.wstring();
    request.args = {L"args", L"", L"two words", L"trailing\\", L"a\\\"b", L"中文", L"\\\\"};
    request.environment = {{L"MINICPP_EXEC_TEST", L"value"}};
    result = execution::Run(request);
    CHECK(result.started && result.reason == execution::Reason::Exited && result.exitCode == 0);
    CHECK(result.out == "\ntwo words\ntrailing\\\na\\\"b\n中文\n\\\\\n");
    CHECK(result.err == "env-ok/cwd-ok");
    request.environment.clear(); request.exe = fixture.wstring();

    for (auto mode : {L"flood", L"stderr-flood"}) {
        request.args = {mode}; request.limits.outputBytes = 12345;
        result = execution::Run(request);
        CHECK(result.reason == execution::Reason::OutputLimit);
        CHECK(result.outputBytes == 12345);
        CHECK(result.out.size() + result.err.size() == 12345);
        CHECK(result.elapsedMs < 1800);
    }
    request.limits.outputBytes = 1024 * 1024;
    std::atomic<bool> floodCancel{false}; request.cancel = &floodCancel;
    request.args = {L"flood"};
    request.onOutput = [&](const std::string&, bool) { floodCancel.store(true); };
    result = execution::Run(request); CHECK(result.reason == execution::Reason::Cancelled);
    CHECK(result.elapsedMs < 1800); request.onOutput = {}; request.cancel = nullptr;
    request.args = {L"sleep"}; request.limits.timeMs = 180;
    result = execution::Run(request); CHECK(result.reason == execution::Reason::TimedOut); CHECK(result.elapsedMs < 1800);

    // An overlapped stdin write must not hold up cancellation if the target never reads.
    request.limits.timeMs = 5000; request.input = std::string(4 * 1024 * 1024, 'i');
    std::atomic<bool> cancel{false}; request.cancel = &cancel;
    std::thread stop([&]() { Sleep(180); cancel.store(true); });
    result = execution::Run(request); stop.join();
    CHECK(result.reason == execution::Reason::Cancelled); CHECK(result.elapsedMs < 1800);
    request.input.clear(); request.cancel = nullptr;
    cancel.store(true); request.cancel = &cancel;
    result = execution::Run(request); CHECK(!result.started); CHECK(result.reason == execution::Reason::Cancelled);
    request.cancel = nullptr;

    request.args = {L"memory"}; request.limits.memoryBytes = 64 * 1024 * 1024;
    result = execution::Run(request); CHECK(result.reason == execution::Reason::MemoryLimit); CHECK(result.elapsedMs < 4000);
    request.limits.memoryBytes = 0;
    request.args = {L"exit7"}; result = execution::Run(request);
    CHECK(result.reason == execution::Reason::Exited); CHECK(result.exitCode == 7);
    request.args = {L"crash"}; result = execution::Run(request); CHECK(result.exitCode == EXCEPTION_ACCESS_VIOLATION);

    for (int scenario = 0; scenario < 3; ++scenario) {
        auto mode = scenario == 1 ? L"tree-exit" : L"tree";
        fs::path record = dir / (L"tree-" + std::to_wstring(scenario) + L".txt");
        request.args = {mode, record.wstring()}; request.limits.timeMs = scenario == 0 ? 700 : 2000;
        std::atomic<bool> treeCancel{false}; std::thread treeStop;
        if (scenario == 2) {
            request.cancel = &treeCancel;
            treeStop = std::thread([&]() { Sleep(500); treeCancel.store(true); });
        }
        result = execution::Run(request);
        if (treeStop.joinable()) treeStop.join();
        request.cancel = nullptr;
        CHECK(result.reason == (scenario == 0 ? execution::Reason::TimedOut : scenario == 1 ? execution::Reason::Exited : execution::Reason::Cancelled));
        std::ifstream file(record); DWORD pid; size_t count = 0;
        while (file >> pid) {
            ++count;
            HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
            CHECK(!process || WaitForSingleObject(process, 0) == WAIT_OBJECT_0);
            if (process) CloseHandle(process);
        }
        CHECK(count == 3);
    }
    // Streaming mode must remain bounded without retaining a second copy of the output.
    request.args = {L"echo"}; request.input = "stream input"; request.limits.timeMs = 2000;
    request.capture = false; size_t streamed = 0;
    request.onOutput = [&](const std::string& text, bool) { streamed += text.size(); };
    result = execution::Run(request);
    CHECK(result.out.empty() && result.err.empty()); CHECK(streamed == result.outputBytes && streamed > 0);
    request.capture = true; request.onOutput = {}; request.input.clear();

    DWORD handlesBefore = 0, handlesAfter = 0;
    GetProcessHandleCount(GetCurrentProcess(), &handlesBefore);
    for (int i = 0; i < 20; ++i) { request.args = {L"exit7"}; result = execution::Run(request); CHECK(result.exitCode == 7); }
    GetProcessHandleCount(GetCurrentProcess(), &handlesAfter);
    CHECK(handlesAfter <= handlesBefore + 1);
    std::error_code ec;
    auto expectedParent = fs::temp_directory_path();
    if (expectedParent.filename().empty()) expectedParent = expectedParent.parent_path();
    if (dir.parent_path() == expectedParent && dir.filename().wstring().find(L"MiniCPP execution ") == 0)
        fs::remove_all(dir, ec);
    else { ++failures; std::cout << "Refusing unexpected cleanup path\n"; }
    CHECK(!ec);
    std::cout << "Execution checks: " << passed << " passed, " << failures << " failed\n";
    return failures ? 1 : 0;
}
