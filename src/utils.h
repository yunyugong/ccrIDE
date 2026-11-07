#pragma once
// MiniCPP - 通用工具
#include <string>
#include <vector>
#include <windows.h>
#include "execution.h"

namespace utils {

// ---- 编码转换 ----
std::string  WToU8(const std::wstring& w);
std::wstring U8ToW(const std::string& s);
bool         IsValidUtf8(const std::string& s);      // 严格 UTF-8 校验
std::string  GbkToUtf8(const std::string& s);        // CP936 -> UTF-8
std::string  Utf8ToGbk(const std::string& s);        // UTF-8 -> CP936
std::string  Sha256Hex(const std::string& data);     // SHA-256 摘要 (64 字符小写 hex)

// ---- 文件读写 (UTF-8) ----
bool WriteFileU8(const std::wstring& path, const std::string& content);
bool ReadFileU8(const std::wstring& path, std::string& out);
unsigned long long FileSize(const std::wstring& path);   // 不存在返回 0

// ---- 路径 ----
std::wstring GetExeDir();                       // 可执行文件所在目录
std::wstring DirName(const std::wstring& path); // 所在目录 (不含文件名)
std::wstring FileBase(const std::wstring& path); // 文件名部分
std::wstring ReplaceExt(const std::wstring& path, const std::wstring& newExt); // 换扩展名
std::wstring QuoteArg(const std::wstring& s);    // 命令行参数加引号

// ---- 驱动器选择: D > E > F, 禁止 C, 返回第一个可写盘 ----
bool SelectSaveDir(std::wstring& out);

// ---- 递归查找 g++.exe ----
bool FindGpp(const std::vector<std::wstring>& roots, std::wstring& out);
std::vector<std::wstring> CompilerSearchRoots();
std::vector<std::wstring> FindGpps(const std::vector<std::wstring>& roots, std::atomic<bool>* cancel = nullptr);

// ---- 运行进程: 输出重定向到文件, 超时强杀 ----
struct RunResult {
    bool   ok        = false;   // 进程成功创建
    bool   timedOut  = false;
    int    exitCode  = -1;
    std::string out;            // stdout 内容
    std::string err;            // stderr 内容
    std::string error;          // 创建失败原因
    execution::Reason reason = execution::Reason::LaunchFailed;
    ULONGLONG elapsedMs = 0;
    size_t peakMemoryBytes = 0;
};
RunResult RunProcess(const std::wstring& exe,
                     const std::vector<std::wstring>& args,
                     const std::wstring& cwd,
                     const std::wstring& outFile,
                     const std::wstring& errFile,
                     DWORD timeoutMs,
                     bool noWindow,
                     std::atomic<bool>* cancel = nullptr);

// ---- 时间戳 backup_YYYYMMDD_HHMMSS ----
std::wstring BackupName();

// ---- 对话框 ----
// 返回 true 且 outPath 有值表示确定; 返回 false 表示取消
bool SaveFileDialog(const std::wstring& initialDir, const std::wstring& defName, std::wstring& outPath);
bool OpenFileDialog(const std::wstring& initialDir, std::wstring& outPath);

// ---- HTTP (WinINet) ----
// GET 一个 URL, 成功返回 true; out 为响应体
bool HttpGet(const std::string& url, std::string& out, std::string& errMsg,const std::string& apiKey = {});
// POST JSON, 返回响应体
bool HttpPostJson(const std::string& url, const std::string& jsonBody, const std::string& apiKey, std::string& out, std::string& errMsg);
// POST JSON 流式 (SSE): chunkCb 返回增量文本片段; done 为 true 时表示结束 (最后一次回调)
// chunkCb 返回 false 可提前中止流
bool HttpPostJsonStream(const std::string& url, const std::string& jsonBody, const std::string& apiKey,
                        std::string& errMsg,
                        bool(*chunkCb)(void* user, const char* text, size_t len, bool done),
                        void* user);

} // namespace utils
