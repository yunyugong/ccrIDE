#include "utils.h"
#include <shlwapi.h>
#include <commdlg.h>
#include <wininet.h>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <set>
#include <algorithm>
#include <cwctype>
#include <nlohmann/json.hpp>

using nlohmann::json;
namespace fs = std::filesystem;

namespace utils {

std::string WToU8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

std::wstring U8ToW(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

bool IsValidUtf8(const std::string& s) {
    if (s.empty()) return true;
    // MB_ERR_INVALID_CHARS: 遇非法序列返回 0 (严格校验)
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                               s.data(), (int)s.size(), nullptr, 0) > 0;
}

std::string GbkToUtf8(const std::string& s) {
    if (s.empty()) return s;
    int wlen = MultiByteToWideChar(936, 0, s.data(), (int)s.size(), nullptr, 0);
    if (wlen <= 0) return s;
    std::wstring w((size_t)wlen, L'\0');
    MultiByteToWideChar(936, 0, s.data(), (int)s.size(), &w[0], wlen);
    return WToU8(w);
}

std::string Utf8ToGbk(const std::string& s) {
    if (s.empty()) return s;
    std::wstring w = U8ToW(s);
    if (w.empty()) return s;
    int glen = WideCharToMultiByte(936, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (glen <= 0) return s;
    std::string g((size_t)glen, '\0');
    WideCharToMultiByte(936, 0, w.data(), (int)w.size(), &g[0], glen, nullptr, nullptr);
    return g;
}

// ---------------- SHA-256 (FIPS 180-4, 用于插件下载完整性校验) ----------------
namespace sha256impl {
    constexpr uint32_t K[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };
    inline uint32_t Rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
    inline void Process(uint32_t st[8], const uint8_t blk[64]) {
        uint32_t w[64];
        for (int i = 0; i < 16; i++)
            w[i] = (uint32_t)blk[i*4] << 24 | (uint32_t)blk[i*4+1] << 16 |
                   (uint32_t)blk[i*4+2] << 8 | (uint32_t)blk[i*4+3];
        for (int i = 16; i < 64; i++) {
            uint32_t s0 = Rotr(w[i-15], 7) ^ Rotr(w[i-15], 18) ^ (w[i-15] >> 3);
            uint32_t s1 = Rotr(w[i-2], 17) ^ Rotr(w[i-2], 19) ^ (w[i-2] >> 10);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }
        uint32_t a=st[0],b=st[1],c=st[2],d=st[3],e=st[4],f=st[5],g=st[6],h=st[7];
        for (int i = 0; i < 64; i++) {
            uint32_t S1 = Rotr(e,6) ^ Rotr(e,11) ^ Rotr(e,25);
            uint32_t ch = (e & f) ^ (~e & g);
            uint32_t t1 = h + S1 + ch + K[i] + w[i];
            uint32_t S0 = Rotr(a,2) ^ Rotr(a,13) ^ Rotr(a,22);
            uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t t2 = S0 + maj;
            h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
        }
        st[0]+=a; st[1]+=b; st[2]+=c; st[3]+=d; st[4]+=e; st[5]+=f; st[6]+=g; st[7]+=h;
    }
}

std::string Sha256Hex(const std::string& data) {
    using namespace sha256impl;
    uint32_t st[8] = { 0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                       0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19 };
    size_t n = data.size();
    std::string padded = data;
    padded.push_back((char)0x80);
    while (padded.size() % 64 != 56) padded.push_back('\0');
    uint64_t bits = (uint64_t)n * 8;
    for (int i = 7; i >= 0; i--) padded.push_back((char)((bits >> (i * 8)) & 0xFF));
    for (size_t off = 0; off < padded.size(); off += 64)
        Process(st, (const uint8_t*)padded.data() + off);
    char out[65];
    for (int i = 0; i < 8; i++) snprintf(out + i * 8, 9, "%08x", st[i]);
    return std::string(out, 64);
}

bool WriteFileU8(const std::wstring& path, const std::string& content) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) return false;
    bool ok = fwrite(content.data(), 1, content.size(), f) == content.size();
    fclose(f);
    return ok;
}

bool ReadFileU8(const std::wstring& path, std::string& out) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.assign(sz > 0 ? (size_t)sz : 0, 0);
    if (sz > 0 && fread(&out[0], 1, (size_t)sz, f) != (size_t)sz) {
        fclose(f);
        return false;
    }
    fclose(f);
    return true;
}

unsigned long long FileSize(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) return 0;
    return ((unsigned long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
}

std::wstring GetExeDir() {
    wchar_t buf[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return DirName(buf);
}

std::wstring DirName(const std::wstring& path) {
    fs::path p(path);
    auto parent = p.parent_path();
    return parent.empty() ? L"." : parent.wstring();
}

std::wstring FileBase(const std::wstring& path) {
    fs::path p(path);
    return p.filename().wstring();
}

std::wstring ReplaceExt(const std::wstring& path, const std::wstring& newExt) {
    fs::path p(path);
    p.replace_extension(newExt);
    return p.wstring();
}

std::wstring QuoteArg(const std::wstring& s) {
    std::wstring result = L"\"";
    size_t slashes = 0;
    for (wchar_t c : s) {
        if (c == L'\\') { ++slashes; continue; }
        if (c == L'"') { result.append(slashes * 2 + 1, L'\\'); result += c; }
        else { result.append(slashes, L'\\'); result += c; }
        slashes = 0;
    }
    result.append(slashes * 2, L'\\');
    result += L'"';
    return result;
}

bool SelectSaveDir(std::wstring& out) {
    for (const wchar_t* d : { L"D:\\", L"E:\\", L"F:\\" }) {
        if (GetFileAttributesW(d) == INVALID_FILE_ATTRIBUTES) continue;
        std::wstring test = d + std::wstring(L"MiniCPP_test");
        HANDLE h = CreateFileW(test.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            CloseHandle(h);
            DeleteFileW(test.c_str());
            out = d;
            return true;
        }
    }
    return false;
}

std::vector<std::wstring> CompilerSearchRoots() {
    std::vector<std::wstring> roots;
    wchar_t drives[512]{};DWORD n=GetLogicalDriveStringsW(512,drives);
    if(n && n<512)for(const wchar_t* drive=drives;*drive;drive+=wcslen(drive)+1) {
        UINT type=GetDriveTypeW(drive);
        if(type==DRIVE_FIXED || type==DRIVE_REMOVABLE)roots.emplace_back(drive);
    }
    return roots;
}
std::vector<std::wstring> FindGpps(const std::vector<std::wstring>& roots,std::atomic<bool>* cancel) {
    std::vector<std::wstring> pending,found;std::set<std::wstring> visited,files;
    auto key=[](std::wstring p){std::replace(p.begin(),p.end(),L'/',L'\\');std::transform(p.begin(),p.end(),p.begin(),towlower);return p;};
    auto extended=[](const std::wstring& p){return p.rfind(L"\\\\?\\",0)==0 ? p : (p.rfind(L"\\\\",0)==0 ? L"\\\\?\\UNC\\"+p.substr(2) : L"\\\\?\\"+p);};
    for(const auto& root:roots) {
        std::error_code ec;auto absolute=fs::absolute(root,ec).lexically_normal().wstring();
        if(!ec)pending.push_back(absolute);
    }
    while(!pending.empty() && !(cancel && cancel->load())) {
        auto dir=std::move(pending.back());pending.pop_back();
        if(!visited.insert(key(dir)).second)continue;
        DWORD attrs=GetFileAttributesW(extended(dir).c_str());
        if(attrs==INVALID_FILE_ATTRIBUTES || !(attrs&FILE_ATTRIBUTE_DIRECTORY) || (attrs&FILE_ATTRIBUTE_REPARSE_POINT))continue;
        if(dir.back()!=L'\\')dir+=L'\\';
        WIN32_FIND_DATAW entry{};HANDLE find=FindFirstFileW((extended(dir)+L"*").c_str(),&entry);
        if(find==INVALID_HANDLE_VALUE)continue;
        do {
            if(cancel && cancel->load())break;
            std::wstring name=entry.cFileName;
            if(name==L"." || name==L".." || (entry.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT))continue;
            auto full=dir+name;
            if(entry.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)pending.push_back(full);
            else if(_wcsicmp(name.c_str(),L"g++.exe")==0 && files.insert(key(full)).second)found.push_back(full);
        }while(FindNextFileW(find,&entry));
        FindClose(find);
    }
    std::sort(found.begin(),found.end(),[&](const auto& a,const auto& b){return key(a)<key(b);});
    return found;
}
bool FindGpp(const std::vector<std::wstring>& roots,std::wstring& out) {
    auto found=FindGpps(roots);if(found.empty())return false;out=found.front();return true;
}

std::wstring PathWithExecutableDirectory(const std::wstring& executable) {
    wchar_t path[32768]{};
    DWORD length=GetEnvironmentVariableW(L"PATH",path,32768);
    std::wstring inherited=(length&&length<32768)?std::wstring(path,length):L"";
    auto directory=DirName(executable);
    return directory.empty()?inherited:directory+(inherited.empty()?L"":L";"+inherited);
}

RunResult RunProcess(const std::wstring& exe,
                     const std::vector<std::wstring>& args,
                     const std::wstring& cwd,
                     const std::wstring& outFile,
                     const std::wstring& errFile,
                     DWORD timeoutMs, bool noWindow, std::atomic<bool>* cancel,execution::Lane lane,const std::string& label,const execution::Task& task) {
    execution::Request request;request.lane=lane;request.label=label;request.task=task;
    request.exe = exe; request.args = args; request.cwd = cwd;
    request.limits.timeMs = timeoutMs;
    request.noWindow = noWindow; request.cancel = cancel;
    request.environment[L"PATH"] = PathWithExecutableDirectory(exe);
    auto executed = execution::Run(request);
    RunResult result;
    result.task=executed.task;result.queuedMs=executed.queuedMs;result.cwd=executed.cwd;
    result.ok = executed.started;
    result.reason = executed.reason;
    result.timedOut = executed.reason == execution::Reason::TimedOut;
    result.exitCode = executed.reason == execution::Reason::Exited ? (int)executed.exitCode : -1;
    result.elapsedMs = executed.elapsedMs; result.peakMemoryBytes = executed.peakMemoryBytes;
    result.out = std::move(executed.out); result.err = std::move(executed.err);
    result.error = executed.error;
    if (executed.reason != execution::Reason::Exited) {
        if (!result.error.empty()) result.error += " ";
        result.error += execution::ReasonText(executed.reason);
        result.err += "\n[执行] " + result.error + "\n";
    }
    // Logs are bounded by the execution limit rather than unbounded file redirection.
    if ((!outFile.empty()&&!WriteFileU8(outFile, result.out)) || (!errFile.empty()&&!WriteFileU8(errFile, result.err))) {
        result.error += " 无法保存执行日志";
        result.reason = execution::Reason::IoError;
        result.exitCode = -1;
        result.err += "\n[执行] 无法保存执行日志\n";
    }
    return result;
}

std::wstring BackupName() {
    wchar_t buf[64];
    time_t t = time(nullptr);
    struct tm tmv;
    localtime_s(&tmv, &t);
    wcsftime(buf, 64, L"backup_%Y%m%d_%H%M%S.cpp", &tmv);
    return buf;
}

bool SaveFileDialog(const std::wstring& initialDir, const std::wstring& defName, std::wstring& outPath) {
    wchar_t fileBuf[MAX_PATH] = {0};
    wcscpy_s(fileBuf, defName.c_str());
    wchar_t dirBuf[MAX_PATH] = {0};
    if (!initialDir.empty()) wcscpy_s(dirBuf, initialDir.c_str());

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = nullptr;
    ofn.lpstrFilter = L"C++ 源文件 (*.cpp;*.c;*.h;*.hpp)\0*.cpp;*.c;*.h;*.hpp\0所有文件 (*.*)\0*.*\0";
    ofn.lpstrFile = fileBuf;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrInitialDir = dirBuf;
    ofn.lpstrDefExt = L"cpp";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameW(&ofn)) return false;
    outPath = fileBuf;
    return true;
}

bool OpenFileDialog(const std::wstring& initialDir, std::wstring& outPath) {
    wchar_t fileBuf[MAX_PATH] = {0};
    wchar_t dirBuf[MAX_PATH] = {0};
    if (!initialDir.empty()) wcscpy_s(dirBuf, initialDir.c_str());

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = nullptr;
    ofn.lpstrFilter = L"C++ 源文件 (*.cpp;*.c;*.h;*.hpp)\0*.cpp;*.c;*.h;*.hpp\0所有文件 (*.*)\0*.*\0";
    ofn.lpstrFile = fileBuf;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrInitialDir = dirBuf;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&ofn)) return false;
    outPath = fileBuf;
    return true;
}

bool PreferencesFileDialog(bool save,const std::wstring& initialDir,std::wstring& outPath) {
    wchar_t file[32768]=L"ccrIDE-preferences.json";
    OPENFILENAMEW dialog{};dialog.lStructSize=sizeof(dialog);
    dialog.lpstrFilter=L"ccrIDE 偏好 (*.json)\0*.json\0所有文件 (*.*)\0*.*\0";
    dialog.lpstrFile=file;dialog.nMaxFile=32768;
    dialog.lpstrInitialDir=initialDir.empty()?nullptr:initialDir.c_str();
    dialog.lpstrDefExt=L"json";dialog.Flags=OFN_EXPLORER|OFN_PATHMUSTEXIST|(save?OFN_OVERWRITEPROMPT:OFN_FILEMUSTEXIST);
    if(!(save?GetSaveFileNameW(&dialog):GetOpenFileNameW(&dialog)))return false;
    outPath=file;return true;
}

// ================= HTTP =================

// 超时: 连接/发送/接收各 15 秒 (接收超时是数据块间隔, 非总时长)
static void SetInetTimeouts(HINTERNET inet) {
    DWORD t = 15000;
    InternetSetOptionW(inet, INTERNET_OPTION_CONNECT_TIMEOUT,  &t, sizeof(t));
    InternetSetOptionW(inet, INTERNET_OPTION_SEND_TIMEOUT,     &t, sizeof(t));
    InternetSetOptionW(inet, INTERNET_OPTION_RECEIVE_TIMEOUT,  &t, sizeof(t));
}

// 请求已发送成功不代表业务成功: 4xx/5xx 一律按失败处理并给出状态码
// (此前 404 等会"成功"返回错误页内容, 掩盖了真实失败原因)
static bool HttpStatusOk(HINTERNET req, std::string& errMsg) {
    DWORD code = 0, len = sizeof(code);
    if (!HttpQueryInfoW(req, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER,
                        &code, &len, nullptr))
        return true;                        // 查询不到就放行, 由读取阶段决定
    if (code >= 400) {
        // 附带服务器返回的错误文本 (截取尾部约 160 字节, 便于定位原因)
        std::string tail;
        char buf[1024];
        DWORD got = 0;
        while (InternetReadFile(req, buf, sizeof(buf), &got) && got > 0)
            tail.append(buf, got);
        size_t keep = 160;
        if (tail.size() > keep) tail = tail.substr(tail.size() - keep);
        auto trim = [&](std::string s) {
            size_t a = s.find_first_not_of(" \t\r\n");
            size_t b = s.find_last_not_of(" \t\r\n");
            return (a == std::string::npos) ? std::string() : s.substr(a, b - a + 1);
        };
        tail = trim(tail);
        errMsg = "HTTP " + std::to_string(code);
        if (!tail.empty()) errMsg += ": " + tail;
        return false;
    }
    return true;
}

static bool UrlToParts(const std::string& url,std::string& scheme,std::string& host,
                       INTERNET_PORT& port,std::string& path) {
    size_t delimiter=url.find("://");if(delimiter==std::string::npos)return false;
    scheme=url.substr(0,delimiter);if(scheme!="http" && scheme!="https")return false;
    size_t slash=url.find('/',delimiter+3);
    std::string authority=url.substr(delimiter+3,slash==std::string::npos ? slash : slash-delimiter-3);
    path=slash==std::string::npos ? "/" : url.substr(slash);
    std::string portText;
    if(!authority.empty() && authority.front()=='[') {
        size_t bracket=authority.find(']');if(bracket==std::string::npos)return false;
        host=authority.substr(1,bracket-1);
        if(bracket+1<authority.size()) {if(authority[bracket+1]!=':')return false;portText=authority.substr(bracket+2);if(portText.empty())return false;}
    } else {
        size_t colon=authority.rfind(':');
        host=colon==std::string::npos ? authority : authority.substr(0,colon);
        if(colon!=std::string::npos) {portText=authority.substr(colon+1);if(portText.empty() || host.find(':')!=std::string::npos)return false;}
    }
    if(host.empty() || host.find('@')!=std::string::npos)return false;
    port=scheme=="https" ? 443 : 80;
    if(!portText.empty()) {
        char* end=nullptr;long value=strtol(portText.c_str(),&end,10);
        if(*end || value<1 || value>65535)return false;port=(INTERNET_PORT)value;
    }
    return true;
}

// 本机回环地址是否应绕过系统代理
// 背景: 用 PRECONFIG 时, 若系统设了全局代理(Clash/公司代理等), 连 127.0.0.1 的
// Ollama 也会被送进代理 -> 12029 无法连接; 回环地址直连即可。
static bool IsLoopbackHost(const std::string& host) {
    return host == "127.0.0.1" || host == "::1" || host == "localhost" ||
           host.rfind("localhost.", 0) == 0;
}

// 打开会话: 回环 -> 直连(免代理); 否则按系统代理配置
static HINTERNET OpenInet(const std::string& host) {
    return InternetOpenW(L"ccrIDE/0.3.0",
                         IsLoopbackHost(host) ? INTERNET_OPEN_TYPE_DIRECT
                                              : INTERNET_OPEN_TYPE_PRECONFIG,
                         nullptr, nullptr, 0);
}

bool HttpGet(const std::string& url, std::string& out, std::string& errMsg,const std::string& apiKey) {
    std::string scheme, host, path;
    INTERNET_PORT port;
    if (!UrlToParts(url, scheme, host, port, path)) {
        errMsg = "URL 格式错误";
        return false;
    }
    HINTERNET inet = OpenInet(host);
    if (!inet) { errMsg = "InternetOpen 失败"; return false; }
    SetInetTimeouts(inet);
    DWORD flags = INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE;
    if (scheme == "https") flags |= INTERNET_FLAG_SECURE;
    HINTERNET conn = InternetConnectW(inet, U8ToW(host).c_str(), port, nullptr, nullptr,
                                      INTERNET_SERVICE_HTTP, 0, 0);
    if (!conn) { errMsg = "InternetConnect 失败"; InternetCloseHandle(inet); return false; }
    HINTERNET req = HttpOpenRequestW(conn, L"GET", U8ToW(path).c_str(), nullptr, nullptr, nullptr,
                                     flags, 0);
    if (!req) { errMsg = "HttpOpenRequest 失败"; InternetCloseHandle(conn); InternetCloseHandle(inet); return false; }
    std::wstring headers=apiKey.empty() ? L"" : L"Authorization: Bearer "+U8ToW(apiKey)+L"\r\n";
    if (!HttpSendRequestW(req,headers.empty() ? nullptr : headers.c_str(),(DWORD)headers.size(),nullptr,0)) {
        errMsg = "HttpSendRequest 失败: " + WToU8(GetLastError() ? std::to_wstring(GetLastError()) : L"");
        InternetCloseHandle(req); InternetCloseHandle(conn); InternetCloseHandle(inet);
        return false;
    }
    if (!HttpStatusOk(req, errMsg)) {
        InternetCloseHandle(req); InternetCloseHandle(conn); InternetCloseHandle(inet);
        return false;
    }
    char buf[8192];
    DWORD read = 0;
    out.clear();
    while (InternetReadFile(req, buf, sizeof(buf), &read) && read > 0) {
        out.append(buf, read);
    }
    InternetCloseHandle(req); InternetCloseHandle(conn); InternetCloseHandle(inet);
    return true;
}

bool HttpPostJson(const std::string& url, const std::string& jsonBody, const std::string& apiKey,
                  std::string& out, std::string& errMsg) {
    std::string scheme, host, path;
    INTERNET_PORT port;
    if (!UrlToParts(url, scheme, host, port, path)) {
        errMsg = "URL 格式错误";
        return false;
    }
    HINTERNET inet = OpenInet(host);
    if (!inet) { errMsg = "InternetOpen 失败"; return false; }
    SetInetTimeouts(inet);
    DWORD flags = INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_KEEP_CONNECTION;
    if (scheme == "https") flags |= INTERNET_FLAG_SECURE;
    HINTERNET conn = InternetConnectW(inet, U8ToW(host).c_str(), port, nullptr, nullptr,
                                      INTERNET_SERVICE_HTTP, 0, 0);
    if (!conn) { errMsg = "InternetConnect 失败"; InternetCloseHandle(inet); return false; }
    HINTERNET req = HttpOpenRequestW(conn, L"POST", U8ToW(path).c_str(), nullptr, nullptr, nullptr,
                                     flags, 0);
    if (!req) { errMsg = "HttpOpenRequest 失败"; InternetCloseHandle(conn); InternetCloseHandle(inet); return false; }

    std::string headers = "Content-Type: application/json\r\nAccept: text/event-stream\r\n";
    if (!apiKey.empty()) headers += "Authorization: Bearer " + apiKey + "\r\n";

    // 宽字符头部: 长度以字符计 (原实现把 UTF-8 字节数当长度, apiKey 含非 ASCII 时会算错)
    std::wstring wHeaders = U8ToW(headers);
    if (!HttpSendRequestW(req, wHeaders.c_str(), (DWORD)wHeaders.size(),
                          (LPVOID)jsonBody.data(), (DWORD)jsonBody.size())) {
        errMsg = "HttpSendRequest 失败: " + WToU8(GetLastError() ? std::to_wstring(GetLastError()) : L"");
        InternetCloseHandle(req); InternetCloseHandle(conn); InternetCloseHandle(inet);
        return false;
    }
    if (!HttpStatusOk(req, errMsg)) {
        InternetCloseHandle(req); InternetCloseHandle(conn); InternetCloseHandle(inet);
        return false;
    }
    char buf[8192];
    DWORD read = 0;
    out.clear();
    while (InternetReadFile(req, buf, sizeof(buf), &read) && read > 0) {
        out.append(buf, read);
    }
    InternetCloseHandle(req); InternetCloseHandle(conn); InternetCloseHandle(inet);
    return true;
}

// 解析 SSE 流中的一行 "data: {...}" 或 "data: [DONE]"
// 返回 true 表示提取到了增量文本; done 设置是否结束
static bool ParseSseLine(const std::string& line, std::string& outText, bool& done) {
    done = false;
    outText.clear();
    // "data: " 前缀 (冒号后可能有空格)
    const char* prefix = "data:";
    size_t plen = 5;
    if (line.size() < plen) return false;
    if (line.compare(0, plen, prefix) != 0) return false;
    size_t p = plen;
    while (p < line.size() && (line[p] == ' ' || line[p] == '\t')) p++;
    std::string payload = line.substr(p);
    // 去尾部 \r
    if (!payload.empty() && payload.back() == '\r') payload.pop_back();
    if (payload == "[DONE]") { done = true; return false; }
    if (payload.empty()) return false;
    try {
        auto j = nlohmann::json::parse(payload);
        // 标准 chat/completions: choices[0].delta.content
        if (j.contains("choices") && !j["choices"].empty()) {
            auto& c0 = j["choices"][0];
            std::string text;
            if (c0.contains("delta") && c0["delta"].contains("content") && !c0["delta"]["content"].is_null())
                text = c0["delta"]["content"].get<std::string>();
            else if (c0.contains("message") && c0["message"].contains("content"))
                text = c0["message"]["content"].get<std::string>();
            else if (c0.contains("text"))
                text = c0["text"].get<std::string>();
            if (!text.empty()) { outText = text; return true; }
        }
        // 兼容某些非标准接口: 直接返回 text / content 字段
        if (j.contains("content")) { outText = j["content"].get<std::string>(); return true; }
        if (j.contains("text"))    { outText = j["text"].get<std::string>();    return true; }
    } catch (...) {}
    return false;
}

bool HttpPostJsonStream(const std::string& url, const std::string& jsonBody, const std::string& apiKey,
                        std::string& errMsg,
                        bool(*chunkCb)(void* user, const char* text, size_t len, bool done),
                        void* user) {
    std::string scheme, host, path;
    INTERNET_PORT port;
    if (!UrlToParts(url, scheme, host, port, path)) {
        errMsg = "URL 格式错误";
        return false;
    }
    HINTERNET inet = OpenInet(host);
    if (!inet) { errMsg = "InternetOpen 失败"; return false; }
    SetInetTimeouts(inet);
    DWORD flags = INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_KEEP_CONNECTION;
    if (scheme == "https") flags |= INTERNET_FLAG_SECURE;
    HINTERNET conn = InternetConnectW(inet, U8ToW(host).c_str(), port, nullptr, nullptr,
                                      INTERNET_SERVICE_HTTP, 0, 0);
    if (!conn) { errMsg = "InternetConnect 失败"; InternetCloseHandle(inet); return false; }
    HINTERNET req = HttpOpenRequestW(conn, L"POST", U8ToW(path).c_str(), nullptr, nullptr, nullptr,
                                     flags, 0);
    if (!req) { errMsg = "HttpOpenRequest 失败"; InternetCloseHandle(conn); InternetCloseHandle(inet); return false; }

    std::string headers = "Content-Type: application/json\r\nAccept: text/event-stream\r\n";
    if (!apiKey.empty()) headers += "Authorization: Bearer " + apiKey + "\r\n";

    // 宽字符头部: 长度以字符计 (原实现把 UTF-8 字节数当长度, apiKey 含非 ASCII 时会算错)
    std::wstring wHeaders = U8ToW(headers);
    if (!HttpSendRequestW(req, wHeaders.c_str(), (DWORD)wHeaders.size(),
                          (LPVOID)jsonBody.data(), (DWORD)jsonBody.size())) {
        errMsg = "HttpSendRequest 失败: " + WToU8(GetLastError() ? std::to_wstring(GetLastError()) : L"");
        InternetCloseHandle(req); InternetCloseHandle(conn); InternetCloseHandle(inet);
        return false;
    }
    if (!HttpStatusOk(req, errMsg)) {
        InternetCloseHandle(req); InternetCloseHandle(conn); InternetCloseHandle(inet);
        return false;
    }

    char buf[8192];
    DWORD read = 0;
    std::string lineBuf;  // 累积未完整的行
    bool aborted = false;
    bool anyChunk = false;

    while (!aborted && InternetReadFile(req, buf, sizeof(buf), &read) && read > 0) {
        size_t start = 0;
        for (size_t i = 0; i < read; i++) {
            if (buf[i] == '\n') {
                lineBuf.append(buf + start, i - start);
                start = i + 1;
                // SSE 事件: 空行分隔事件; 一行一行处理
                if (!lineBuf.empty()) {
                    std::string outText;
                    bool done = false;
                    if (ParseSseLine(lineBuf, outText, done)) {
                        anyChunk = true;
                        if (!chunkCb(user, outText.data(), outText.size(), false)) { aborted = true; break; }
                    } else if (done) {
                        chunkCb(user, "", 0, true);
                        aborted = true;
                        break;
                    }
                }
                lineBuf.clear();
            }
        }
        if (start < read && !aborted) lineBuf.append(buf + start, read - start);
    }

    // 收尾: lineBuf 还有内容 (服务端未按规范换行结束)
    if (!aborted && !lineBuf.empty()) {
        std::string outText;
        bool done = false;
        if (ParseSseLine(lineBuf, outText, done)) {
            anyChunk = true;
            chunkCb(user, outText.data(), outText.size(), false);
        }
    }
    chunkCb(user, "", 0, true);   // 总是最终 done 回调

    InternetCloseHandle(req); InternetCloseHandle(conn); InternetCloseHandle(inet);
    if (!anyChunk && errMsg.empty()) errMsg = "空流或非 SSE 响应 (请检查模型是否支持流式)";
    return anyChunk || aborted;
}

} // namespace utils
