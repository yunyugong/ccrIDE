#include <windows.h>
#include <wininet.h>
#include <cstdio>
#include <string>

static void logf(const char* fmt, ...) {
    char buf[1024];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    FILE* f = fopen("E:\\cppIDE\\wl_out.txt", "a");
    if (f) { fputs(buf, f); fputc('\n', f); fclose(f); }
}

int main() {
    for (int mode = 0; mode < 2; mode++) {
        DWORD it = mode == 0 ? INTERNET_OPEN_TYPE_DIRECT : INTERNET_OPEN_TYPE_PRECONFIG;
        logf("mode=%d", mode);
        HINTERNET inet = InternetOpenW(L"t", it, nullptr, nullptr, 0);
        if (!inet) { logf("  open fail %lu", GetLastError()); continue; }
        HINTERNET c = InternetConnectW(inet, L"127.0.0.1", 18899, nullptr, nullptr,
                                       INTERNET_SERVICE_HTTP, 0, 0);
        if (!c) { logf("  connect fail %lu", GetLastError()); InternetCloseHandle(inet); continue; }
        HINTERNET r = HttpOpenRequestW(c, L"GET", L"/v1/models", nullptr, nullptr, nullptr,
                                       INTERNET_FLAG_NO_CACHE_WRITE, 0);
        if (!r) { logf("  openreq fail %lu", GetLastError()); InternetCloseHandle(c); InternetCloseHandle(inet); continue; }
        if (!HttpSendRequestW(r, nullptr, 0, nullptr, 0)) {
            logf("  send fail %lu", GetLastError());
            InternetCloseHandle(r); InternetCloseHandle(c); InternetCloseHandle(inet);
            continue;
        }
        char buf[2048]; DWORD rd = 0; std::string body;
        while (InternetReadFile(r, buf, sizeof(buf), &rd) && rd > 0) body.append(buf, rd);
        logf("  OK len=%zu", body.size());
        InternetCloseHandle(r); InternetCloseHandle(c); InternetCloseHandle(inet);
    }
    return 0;
}
