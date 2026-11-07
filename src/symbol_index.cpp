// MiniCPP - 轻量 C++ 符号索引器实现
// 思路 (同小熊猫C++ 的内置解析器): 对源码做注释/字符串剥离后, 用一个
// 花括号栈 + 少量状态机扫描 namespace/class/struct 结构, 收集类的方法与字段。
// 不做完整语法分析, 只保证对常见头文件/用户代码有足够好的提取效果。
#include "symbol_index.h"
#include "utils.h"
#include "app.h"
#include <cctype>
#include <algorithm>
#include <functional>

SymbolIndex& SymbolIndex::I() {
    static SymbolIndex inst;
    return inst;
}

// ------------------------------------------------------------------
// 注释与字符串剥离 (替换为空格)
// ------------------------------------------------------------------
static void StripComments(std::string& s) {
    size_t i = 0, n = s.size();
    while (i < n) {
        char c = s[i];
        if (c == '/' && i + 1 < n && s[i + 1] == '/') {
            while (i < n && s[i] != '\n') s[i++] = ' ';
        } else if (c == '/' && i + 1 < n && s[i + 1] == '*') {
            s[i] = s[i + 1] = ' '; i += 2;
            while (i < n) {
                if (s[i] == '*' && i + 1 < n && s[i + 1] == '/') { s[i] = s[i + 1] = ' '; i += 2; break; }
                if (s[i] != '\n') s[i] = ' ';
                i++;
            }
        } else if (c == '"' || c == '\'') {
            char q = c; s[i++] = ' ';
            while (i < n && s[i] != q) {
                if (s[i] == '\\' && i + 1 < n) { s[i++] = ' '; }
                if (i < n && s[i] != '\n' && s[i] != q) s[i] = ' ';
                i++;
            }
            if (i < n) s[i++] = ' ';
        } else i++;
    }
}

// ------------------------------------------------------------------
// 提取源码中的 #include 头文件名 (供解析系统头时递归跟随)
// ------------------------------------------------------------------
static void ExtractIncludes(const std::string& s, std::vector<std::string>& out) {
    size_t i = 0, n = s.size();
    while (i < n) {
        size_t le = s.find('\n', i);
        if (le == std::string::npos) le = n;
        size_t a = i;
        while (a < le && (s[a] == ' ' || s[a] == '\t')) a++;
        if (a < le && s[a] == '#') {
            size_t b = a + 1;
            while (b < le && (s[b] == ' ' || s[b] == '\t')) b++;
            if (s.compare(b, 7, "include") == 0) {
                b += 7;
                while (b < le && (s[b] == ' ' || s[b] == '\t')) b++;
                if (b < le && (s[b] == '<' || s[b] == '"')) {
                    char close = (s[b] == '<') ? '>' : '"';
                    size_t c = b + 1;
                    while (c < le && s[c] != close) c++;
                    if (c < le && c > b + 1) out.push_back(s.substr(b + 1, c - b - 1));
                }
            }
        }
        i = le + 1;
    }
}

// 成员名过滤: 跳过库内部实现细节 (libstdc++ 的 _M_/_S_/__ 前缀) 与关键字
static bool IsNoiseMember(const std::string& w) {
    if (w.size() >= 2 && w[0] == '_' && (w[1] == '_' || isupper((unsigned char)w[1])))
        return true;
    static const char* kw[] = {
        "operator", "static_assert", "sizeof", "alignas", "alignof", "decltype",
        "noexcept", "throw", "return", "template", "typedef", "using", "friend",
        "enum", "constexpr", "consteval", "explicit", "virtual", "inline",
        "mutable", "const", "static", "public", "private", "protected", "typename",
        "extern", "volatile", "register"
    };
    for (auto k : kw) if (w == k) return true;
    return false;
}

// ------------------------------------------------------------------
// 主扫描: 状态机 + 括号栈
//   namespace X {           -> 压 NS 帧
//   class/struct N [: B] {  -> 压 CLS 帧, 注册 ClassInfo
//   其他 '{'                -> OTHER 帧 (函数体/枚举等, 屏蔽内部误判)
// 成员判定: 栈顶为 CLS 且 parenDepth==0 时,
//   标识符后跟 '(' => 方法; 跟 ';' ',' '=' ':' => 字段。
// 别名: using NAME = TYPE ...;  typedef TYPE NAME;  (仅全局/命名空间作用域)
// ------------------------------------------------------------------
void SymbolIndex::ParseText(const std::string& src) {
    std::string t = src;
    StripComments(t);
    const size_t n = t.size();

    enum St {
        Idle, NsName, ClsName, ClsWait, Bases,
        UsName, UsEq, UsType,                   // using NAME = TYPE
        TdType, TdName                          // typedef TYPE NAME
    } st = Idle;
    std::string pendingNs, pendingCls, curBase, aliasFrom, tdType, tdName;
    bool nsPending = false, clsPending = false;
    std::vector<std::string> bases;

    enum FKind { FOther, FNs, FCls };
    struct Frame { FKind kind; std::string clsKey; };
    std::vector<Frame> frames;
    int parenDepth = 0, angleDepth = 0;

    auto isid0 = [&](char c) { return isalpha((unsigned char)c) || c == '_'; };
    auto isid  = [&](char c) { return isalnum((unsigned char)c) || c == '_'; };
    auto flushBase = [&]() {
        if (!curBase.empty()) { bases.push_back(curBase); curBase.clear(); }
    };

    size_t i = 0;
    while (i < n) {
        char c = t[i];
        if (isspace((unsigned char)c)) { i++; continue; }

        if (isid0(c)) {
            size_t j = i + 1;
            while (j < n && isid(t[j])) j++;
            std::string w = t.substr(i, j - i);

            switch (st) {
                case NsName:
                    pendingNs = w; nsPending = true; st = Idle;
                    i = j; continue;
                case ClsName:
                    pendingCls = w; clsPending = true; st = ClsWait;
                    i = j; continue;
                case ClsWait: {
                    static const char* sk[] = { "final","override","alignas" };
                    bool skipW = false;
                    for (auto k : sk) if (w == k) { skipW = true; break; }
                    if (!skipW) st = Idle;      // 属性宏等: 停留等待 '{'/';'/':'
                    i = j; continue;
                }
                case Bases: {
                    static const char* sk[] = { "public","protected","private","virtual","final","override" };
                    bool skipW = false;
                    for (auto k : sk) if (w == k) { skipW = true; break; }
                    if (!skipW) curBase = w;    // 每段取最后的标识符 (:: 链/模板已略过)
                    i = j; continue;
                }
                case UsName:
                    aliasFrom = w; st = UsEq;
                    i = j; continue;
                case UsEq:
                    if (w != "=") st = Idle;    // 异常序列放弃
                    else st = UsType;
                    i = j; continue;
                case UsType:
                    // 仅记录全局/命名空间作用域的别名 (类内多为模板参数别名, 会污染)
                    if (frames.empty() || frames.back().kind != FCls)
                        m_alias[aliasFrom] = w; // 目标类型短名
                    st = Idle;
                    i = j; continue;
                case TdType: {
                    static const char* tq[] = { "const","volatile","struct","class",
                        "enum","union","unsigned","signed","short","long","typename","static" };
                    bool skip = false;
                    for (auto k : tq) if (w == k) { skip = true; break; }
                    if (!skip) { tdType = w; st = TdName; }
                    i = j; continue;
                }
                case TdName:
                    tdName = w;             // 取最后一个标识符作为别名
                    i = j; continue;
                default: break;
            }

            // ---- Idle ----
            if (w == "namespace") { st = NsName; i = j; continue; }
            if (w == "using")     { st = UsName; i = j; continue; }
            if (w == "typedef")   { st = TdType; tdType.clear(); tdName.clear(); i = j; continue; }
            if (w == "class" || w == "struct") {
                st = ClsName; pendingCls.clear();
                bases.clear(); curBase.clear(); clsPending = false;
                i = j; continue;
            }

            // ---- 成员判定 (栈顶 CLS 且括号深度 0) ----
            if (!frames.empty() && frames.back().kind == FCls && parenDepth == 0) {
                size_t k = j;
                while (k < n && isspace((unsigned char)t[k])) k++;
                char nx = (k < n) ? t[k] : '\0';
                const std::string& clsKey = frames.back().clsKey;
                if (nx == '(' || nx == ';' || nx == ',' || nx == '=' || nx == ':' ||
                    nx == '{' || nx == '<') {
                    if (!w.empty() && w != clsKey && !IsNoiseMember(w)) {  // 跳过构造函数/内部名
                        auto& ci = m_classes[clsKey];
                        bool dup = false;
                        for (auto& m : ci.members) if (m.name == w) { dup = true; break; }
                        if (!dup) {
                            std::string sig;
                            if (nx == '(') {
                                // 提取签名: 从 "(" 平衡扫描到 ")" (限 256 字节, 够参数列表用)
                                int dep = 0; size_t p = k, end = std::string::npos;
                                for (; p < n && p - k < 256; p++) {
                                    char d = t[p];
                                    if (d == '(') dep++;
                                    else if (d == ')') { dep--; if (dep == 0) { end = p + 1; break; } }
                                    else if (d == ';' && dep == 0) break;
                                }
                                if (end != std::string::npos) sig = w + t.substr(k, end - k);
                            }
                            ci.members.push_back({ w, nx == '(', sig });
                        }
                    }
                }
            }
            i = j; continue;
        }

        // ---- 标点 ----
        switch (c) {
            case '{': {
                flushBase();
                Frame f{ FOther, "" };
                if (nsPending) { f.kind = FNs; nsPending = false; }
                else if (clsPending) {
                    f.kind = FCls;
                    ClassInfo ci; ci.name = pendingCls; ci.bases = bases;
                    m_classes[pendingCls] = std::move(ci);
                    f.clsKey = pendingCls;
                    clsPending = false;
                }
                frames.push_back(f);
                st = Idle;
                break;
            }
            case '}':
                flushBase();
                if (!frames.empty()) frames.pop_back();
                st = Idle;
                break;
            case '(':
                parenDepth++;
                if (clsPending || nsPending) { clsPending = nsPending = false; st = Idle; }
                if (st == TdType || st == TdName) st = Idle;   // 函数指针式 typedef: 放弃
                break;
            case ')':
                if (parenDepth > 0) parenDepth--;
                break;
            case '<':
                angleDepth++;
                break;
            case '>':
                if (angleDepth > 0) angleDepth--;
                break;
            case '*':
                if (st == TdType || st == TdName) st = Idle;   // typedef TYPE* NAME: 放弃
                break;
            case ';':
                flushBase();
                if (st == TdName && !tdType.empty() && !tdName.empty() &&
                    (frames.empty() || frames.back().kind != FCls))
                    m_alias[tdName] = tdType;                  // typedef TYPE NAME;
                nsPending = clsPending = false;
                if (st != Bases) st = Idle;
                break;
            case ':':
                if (st == TdType || st == TdName) st = Idle;    // 限定名 typedef: 放弃
                else if (st == ClsWait) st = Bases;
                break;
            case ',':
                if (st == Bases && angleDepth == 0) flushBase();
                break;
            default: break;
        }
        i++;
    }
}

// ------------------------------------------------------------------
// 用户文档: 内容 hash 变了才重新解析
// ------------------------------------------------------------------
void SymbolIndex::ScanDoc(uint64_t uid, const std::string* doc) {
    size_t h = std::hash<std::string>{}(*doc);
    auto it = m_docHash.find(uid);
    if (it != m_docHash.end() && it->second == h) return;
    m_docHash[uid] = h;
    ParseText(*doc);
}

// ------------------------------------------------------------------
// include 路径: g++ -v 解析一次; 失败按 MinGW 目录布局猜测
// ------------------------------------------------------------------
static bool FileExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

void SymbolIndex::EnsureIncludeDirs() {
    if (m_dirsDone) return;
    m_dirsDone = true;
    extern App g_app;
    std::wstring gpp = utils::U8ToW(g_app.settings.gppPath);

    if (!gpp.empty()) {
        // This legacy lazy query is synchronous; do not wait on busy foreground jobs.
        execution::Request query;
        query.exe=gpp;query.cwd=utils::DirName(gpp);
        query.environment[L"PATH"]=utils::PathWithExecutableDirectory(gpp);
        query.args={L"-x",L"c++",L"-E",L"-v",L"nul"};
        query.label="头文件路径查询";query.lane=execution::Lane::Background;
        query.queueTimeoutMs=100;query.limits={6000,0,1024*1024};
        query.cancel=&g_app.shuttingDown;
        auto result=execution::Run(query);
        std::string all=result.err+"\n"+result.out;
        if(!utils::IsValidUtf8(all)){
            int size=MultiByteToWideChar(CP_ACP,0,all.data(),int(all.size()),nullptr,0);
            std::wstring decoded(size,0);
            if(size)MultiByteToWideChar(CP_ACP,0,all.data(),int(all.size()),decoded.data(),size);
            all=utils::WToU8(decoded);
        }
        size_t p1 = all.find("search starts here");
        size_t p2 = all.find("End of search list");
        if (p1 != std::string::npos && p2 != std::string::npos && p2 > p1) {
            std::string seg = all.substr(p1, p2 - p1);
            size_t pos = 0;
            while (pos <= seg.size()) {
                size_t e = seg.find('\n', pos);
                if (e == std::string::npos) { if (pos == seg.size()) break; e = seg.size(); }
                std::string line = seg.substr(pos, e - pos);
                pos = e + 1;
                size_t a = line.find_first_not_of(" \t\r");
                if (a == std::string::npos) continue;
                size_t b = line.find_last_not_of(" \t\r");
                line = line.substr(a, b - a + 1);
                if (line.find("(framework directory)") != std::string::npos) continue;
                auto path=utils::U8ToW(line);DWORD attributes=GetFileAttributesW(path.c_str());
                if(attributes!=INVALID_FILE_ATTRIBUTES&&(attributes&FILE_ATTRIBUTE_DIRECTORY))m_dirs.push_back(path);
            }
        }
    }
    // 兜底: <root>\lib\gcc\<arch>\<ver>\include\c++\<ver>
    if (m_dirs.empty() && !gpp.empty()) {
        std::wstring root = utils::DirName(utils::DirName(gpp));   // 去 bin\g++.exe
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((root + L"\\lib\\gcc\\*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                if (fd.cFileName[0] == L'.') continue;
                std::wstring arch = root + L"\\lib\\gcc\\" + fd.cFileName;
                WIN32_FIND_DATAW fd2;
                HANDLE h2 = FindFirstFileW((arch + L"\\*").c_str(), &fd2);
                if (h2 != INVALID_HANDLE_VALUE) {
                    do {
                        if (!(fd2.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                        if (fd2.cFileName[0] == L'.') continue;
                        std::wstring base = arch + L"\\" + fd2.cFileName +
                                            L"\\include\\c++";
                        for(auto candidate:{base,base+L"\\"+fd.cFileName,base+L"\\backward"}){
                            DWORD attr=GetFileAttributesW(candidate.c_str());
                            if(attr!=INVALID_FILE_ATTRIBUTES&&(attr&FILE_ATTRIBUTE_DIRECTORY))m_dirs.push_back(candidate);
                        }
                    } while (FindNextFileW(h2, &fd2));
                    FindClose(h2);
                }
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        m_dirs.push_back(root + L"\\x86_64-w64-mingw32\\include");
        m_dirs.push_back(root + L"\\include");
    }
}

const std::vector<std::wstring>& SymbolIndex::IncludeDirs() {
    EnsureIncludeDirs();   // 惰性求值: 仅首次解析 g++ -v 输出
    return m_dirs;
}

bool SymbolIndex::ParseSystemHeader(const std::string& headerName, int depth) {
    if (m_parsedHeaders.count(headerName)) return true;
    if (m_noHeader.count(headerName)) return false;
    // 递归跟随 #include 的层数与总量上限 (libstdc++ 头文件互相包含很深)
    if (depth > 4 || m_parsedHeaders.size() >= 800) { m_noHeader.insert(headerName); return false; }
    EnsureIncludeDirs();
    std::wstring wname = utils::U8ToW(headerName);
    for (auto& d : m_dirs) {
        std::wstring p = d + L"\\" + wname;
        if (!FileExists(p)) continue;
        std::string text;
        if (!utils::ReadFileU8(p, text)) break;
        std::string t = text;
        StripComments(t);
        ParseText(t);                       // 解析本体 (类/成员/别名)
        m_parsedHeaders.insert(headerName);
        // 跟随 #include: <vector> 实际定义在 <bits/stl_vector.h>, 必须跟进去才能拿到成员
        std::vector<std::string> incs;
        ExtractIncludes(t, incs);
        for (auto& h : incs) ParseSystemHeader(h, depth + 1);
        return true;
    }
    m_noHeader.insert(headerName);
    return false;
}

const SymbolIndex::ClassInfo* SymbolIndex::FindClass(const std::string& shortName, int depth) {
    if (depth > 3) return nullptr;
    auto it = m_classes.find(shortName);
    if (it != m_classes.end()) return &it->second;
    auto al = m_alias.find(shortName);
    if (al != m_alias.end()) return FindClass(al->second, depth + 1);
    return nullptr;
}

void SymbolIndex::CollectChain(const std::string& shortName, std::vector<SymRef>& out,
                               std::unordered_set<std::string>& seen, int depth) {
    if (depth > 4) return;
    const ClassInfo* ci = FindClass(shortName, depth);
    if (!ci) return;
    for (auto& m : ci->members)
        if (seen.insert(m.name).second) out.push_back(m);
    for (auto& b : ci->bases) CollectChain(b, out, seen, depth + 1);
}

std::vector<SymRef> SymbolIndex::MembersOfType(const std::string& normType,
                                               const std::vector<DocRef>& docs) {
    std::string ty = normType;
    // 常用流对象: 类型声明在 <iostream>, 文档里通常查不到
    static const std::unordered_map<std::string, std::string> objs = {
        {"cout","ostream"}, {"cerr","ostream"}, {"clog","ostream"}, {"cin","istream"},
        {"wcout","wostream"}, {"wcerr","wostream"}, {"wclog","wostream"}, {"wcin","wistream"} };
    auto oi = objs.find(ty);
    if (oi != objs.end()) ty = oi->second;

    for (auto& d : docs) ScanDoc(d.uid, d.text);

    // 未在文档中命中 -> 解析同名系统头文件 (vector/string/map...)
    if (m_classes.find(ty) == m_classes.end() &&
        m_alias.find(ty) == m_alias.end())
        ParseSystemHeader(ty);

    std::vector<SymRef> out;
    std::unordered_set<std::string> seen;
    CollectChain(ty, out, seen, 0);
    return out;
}

// ------------------------------------------------------------------
// #include 递归内联 (模拟 CppPreprocessor): 生成"头文件翻译单元"文本
// ------------------------------------------------------------------
std::vector<std::string> SymbolIndex::ExtractIncludeNames(const std::string& text) {
    std::vector<std::string> out;
    ExtractIncludes(text, out);
    return out;
}

// 路径大小写不敏感比较 (Windows)
static bool WPathEq(const std::wstring& a, const std::wstring& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++) {
        wchar_t x = a[i], y = b[i];
        if (x >= L'A' && x <= L'Z') x += 32;
        if (y >= L'A' && y <= L'Z') y += 32;
        if (x != y) return false;
    }
    return true;
}
static int GetFileId(std::vector<std::wstring>& files, const std::wstring& path) {
    for (size_t i = 0; i < files.size(); i++) if (WPathEq(files[i], path)) return (int)i;
    files.push_back(path);
    return (int)files.size() - 1;
}

// 逐行 Depth-First 展开 #include (在出现位置就地内联, 与真实预处理顺序一致),
// 同时记录每行的来源 (文件 + 行号) 供跳转定义映射。
static void ExpandIncludesInPlace(const std::string& text, const std::wstring& dir,
                                  const std::vector<std::wstring>& dirs,
                                  std::unordered_set<std::string>& visited,
                                  std::string& out, size_t maxBytes, int depth,
                                  std::vector<std::wstring>& files,
                                  std::vector<SymbolIndex::TuSegment>& segs,
                                  int fileId, int lineStart) {
    if (depth > 64 || out.size() >= maxBytes) return;
    size_t i = 0, n = text.size();
    int ln = lineStart;
    while (i < n && out.size() < maxBytes) {
        size_t le = text.find('\n', i);
        if (le == std::string::npos) le = n;
        // 该行是否为 #include
        size_t a = i;
        while (a < le && (text[a] == ' ' || text[a] == '\t')) a++;
        bool isInc = false;
        std::string hname; bool quote = false;
        if (a < le && text[a] == '#') {
            size_t b = a + 1;
            while (b < le && (text[b] == ' ' || text[b] == '\t')) b++;
            if (b + 7 <= le && text.compare(b, 7, "include") == 0) {
                b += 7;
                while (b < le && (text[b] == ' ' || text[b] == '\t')) b++;
                if (b < le && (text[b] == '<' || text[b] == '"')) {
                    quote = (text[b] == '"');
                    char close = quote ? '"' : '>';
                    size_t c = b + 1;
                    while (c < le && text[c] != close) c++;
                    if (c < le && c > b + 1) { hname = text.substr(b + 1, c - b - 1); isInc = true; }
                }
            }
        }
        if (isInc) {
            std::wstring wname = utils::U8ToW(hname);
            std::wstring path; bool found = false;
            if (quote && !dir.empty()) {
                std::wstring p = dir + L"\\" + wname;
                if (FileExists(p)) { path = p; found = true; }
            }
            if (!found) {
                for (auto& d : dirs) {
                    std::wstring p = d + L"\\" + wname;
                    if (FileExists(p)) { path = p; found = true; break; }
                }
            }
            if (found) {
                std::string key = utils::WToU8(path);
                for (auto& c : key) c = (char)tolower((unsigned char)c);
                if (visited.insert(key).second) {
                    std::string ft;
                    if (utils::ReadFileU8(path, ft)) {
                        if (ft.size() > (8u << 20)) ft.resize(8u << 20);
                        int fid = GetFileId(files, path);
                        out += '\n';
                        ExpandIncludesInPlace(ft, utils::DirName(path), dirs, visited,
                                              out, maxBytes, depth + 1, files, segs, fid, 1);
                    }
                }
            }
        } else {
            SymbolIndex::TuSegment sg;
            sg.outStart = out.size();
            sg.outLen = le - i;
            sg.fileId = fileId;
            sg.line = ln;
            segs.push_back(sg);
            out.append(text, i, le - i);
            out += '\n';
        }
        ln++;
        i = le + 1;
    }
}

SymbolIndex::HeaderUnit SymbolIndex::BuildHeaderUnit(const std::string& docText,
                                                     const std::wstring& docPath,
                                                     size_t maxBytes) {
    HeaderUnit u;
    EnsureIncludeDirs();
    if (m_dirs.empty()) return u;
    u.text.reserve(1 << 20);
    std::unordered_set<std::string> visited;
    int docId = docPath.empty() ? -1 : GetFileId(u.files, docPath);
    std::wstring localDir = docPath.empty() ? std::wstring() : utils::DirName(docPath);
    ExpandIncludesInPlace(docText, localDir, m_dirs, visited, u.text, maxBytes, 0,
                          u.files, u.segments, docId, 1);
    return u;
}

bool SymbolIndex::HeaderUnit::Locate(size_t off, std::wstring& file, int& line) const {
    if (segments.empty()) return false;
    auto it = std::upper_bound(segments.begin(), segments.end(), off,
        [](size_t v, const TuSegment& s) { return v < s.outStart; });
    if (it == segments.begin()) return false;
    --it;
    if (it->fileId < 0 || it->fileId >= (int)files.size()) return false;
    file = files[it->fileId];
    line = it->line;
    return true;
}


