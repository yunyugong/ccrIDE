#include <windows.h>
#include <shellapi.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

static void Write(HANDLE handle, const std::string& text) {
    DWORD written; WriteFile(handle, text.data(), (DWORD)text.size(), &written, nullptr);
}
static bool Spawn(const std::wstring& exe, const std::wstring& mode, const std::wstring& file) {
    std::wstring command = L"\"" + exe + L"\" " + mode + L" \"" + file + L"\"";
    STARTUPINFOW startup{sizeof(startup)}; PROCESS_INFORMATION process{};
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &startup, &process)) return false;
    CloseHandle(process.hThread); CloseHandle(process.hProcess); return true;
}
int main() {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    int count; auto args = CommandLineToArgvW(GetCommandLineW(), &count);
    std::vector<std::wstring> argv(args, args + count); LocalFree(args);
    if (count < 2) return 2;
    const auto& mode = argv[1];
    if(mode==L"--interpreter=mi2") {
        wchar_t behavior[64]{}; GetEnvironmentVariableW(L"CCRIDE_GDB_FIXTURE",behavior,64);
        if(std::wstring(behavior)==L"protocol") Write(GetStdHandle(STD_OUTPUT_HANDLE),"^done,broken\n");
        Sleep(30000); return 0;
    }
    if (mode == L"echo") {
        char buf[4096]; DWORD got;
        while (ReadFile(GetStdHandle(STD_INPUT_HANDLE), buf, sizeof(buf), &got, nullptr) && got)
            Write(GetStdHandle(STD_OUTPUT_HANDLE), std::string(buf, got));
        Write(GetStdHandle(STD_ERROR_HANDLE), "stderr marker"); return 0;
    }
    if (mode == L"flood" || mode == L"stderr-flood") {
        HANDLE output = GetStdHandle(mode == L"flood" ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE);
        const std::string text(8192, 'x'); for (;;) Write(output, text);
    }
    if (mode == L"memory") {
        for (;;) {
            void* memory = VirtualAlloc(nullptr, 8 * 1024 * 1024, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (!memory) { Sleep(5000); return 9; }
            memset(memory, 1, 8 * 1024 * 1024);
        }
    }
    if (mode == L"args") {
        for (int i = 2; i < count; ++i) {
            int size = WideCharToMultiByte(CP_UTF8, 0, argv[i].data(), (int)argv[i].size(), nullptr, 0, nullptr, nullptr);
            std::string value(size, 0);
            if (size) WideCharToMultiByte(CP_UTF8, 0, argv[i].data(), (int)argv[i].size(), value.data(), size, nullptr, nullptr);
            Write(GetStdHandle(STD_OUTPUT_HANDLE), value + "\n");
        }
        wchar_t value[256]{}; GetEnvironmentVariableW(L"MINICPP_EXEC_TEST", value, 256);
        Write(GetStdHandle(STD_ERROR_HANDLE), value[0] == L'v' ? "env-ok" : "env-bad");
        if (std::filesystem::exists(L"cwd-marker")) Write(GetStdHandle(STD_ERROR_HANDLE), "/cwd-ok");
        return 0;
    }
    if (mode == L"exit7") return 7;
    if (mode == L"touch") { std::ofstream("started-marker") << "started"; return 0; }
    if (mode == L"crash") { RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr); return 8; }
    if (mode == L"tree" || mode == L"tree-exit" || mode == L"tree-child" || mode == L"tree-grand") {
        if (count != 3) return 3;
        const std::wstring file = argv[2];
        { std::ofstream record(std::filesystem::path(file), std::ios::app); record << GetCurrentProcessId() << '\n'; }
        if (mode != L"tree-grand" && !Spawn(argv[0], mode == L"tree-child" ? L"tree-grand" : L"tree-child", file)) return 4;
        if (mode == L"tree-exit") {
            for (int i = 0; i < 100; ++i) {
                std::ifstream record{std::filesystem::path(file)};
                unsigned pid, n = 0; while (record >> pid) ++n;
                if (n >= 3) return 0;
                Sleep(10);
            }
            return 5;
        }
    }
    Sleep(30000); return 0; // "sleep" deliberately never reads stdin.
}
