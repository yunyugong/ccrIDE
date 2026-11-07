// MiniCPP - 文档级 C++ 语义模型实现
// 做法: 先做一次容忍度较高的词法分析, 再用"语句-级"扫描维护花括号作用域栈,
// 识别常见声明 (变量/参数/函数/类/结构体/枚举/typedef/using), 从而支持
// 作用域感知补全与对象类型推断。不追求完整语法, 但覆盖绝大多数日常代码。
#include "code_model.h"
#include <cctype>
#include <cstring>
#include <algorithm>
#include <functional>

namespace {

const size_t NPOS = (size_t)-1;

bool IsIdStart(char c) { return isalpha((unsigned char)c) || c == '_' || (unsigned char)c >= 0x80; }
bool IsIdChar(char c)  { return isalnum((unsigned char)c) || c == '_' || (unsigned char)c >= 0x80; }

std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}
std::string Collapse(const std::string& s) {
    std::string r; r.reserve(s.size());
    bool sp = false;
    for (char c : s) {
        if (c == '\n' || c == '\r' || c == '\t' || c == ' ') { sp = true; continue; }
        if (sp && !r.empty()) r += ' ';
        sp = false;
        r += c;
    }
    return r;
}
// 整理类型串: 去掉 :: < > * & , ( ) 周围的空格, 保留标识符之间的空格
std::string CleanType(const std::string& s) {
    std::string r; r.reserve(s.size());
    auto punct = [](char x) { return x == ':' || x == '<' || x == '>' || x == '*' || x == '&' || x == ',' || x == '(' || x == ')'; };
    for (size_t i = 0; i < s.size(); i++) {
        char c = s[i];
        if (c == ' ') {
            char prev = r.empty() ? 0 : r.back();
            char next = (i + 1 < s.size()) ? s[i + 1] : 0;
            if (r.empty() || punct(prev) || punct(next)) continue;
            r += ' ';
        } else r += c;
    }
    return r;
}

const std::unordered_set<std::string>& BuiltinSet() {
    static const std::unordered_set<std::string> s = {
        "void","bool","char","short","int","long","float","double","signed","unsigned",
        "wchar_t","char8_t","char16_t","char32_t","size_t","ssize_t","ptrdiff_t",
        "int8_t","int16_t","int32_t","int64_t","uint8_t","uint16_t","uint32_t","uint64_t",
        "auto","nullptr_t",
        "string","wstring","u8string","u16string","u32string","string_view","vector","array",
        "deque","list","forward_list","map","multimap","set","multiset","unordered_map",
        "unordered_set","unordered_multimap","unordered_multiset","stack","queue",
        "priority_queue","pair","tuple","optional","variant","any","function","bitset",
        "complex","valarray","thread","mutex","atomic","unique_ptr","shared_ptr","weak_ptr",
        "fstream","ifstream","ofstream","stringstream","istringstream","ostringstream",
        "ostream","istream","iostream","FILE","time_t","clock_t","va_list","initializer_list",
        "iterator","const_iterator","reverse_iterator","exception","runtime_error","logic_error"
    };
    return s;
}

const std::unordered_set<std::string>& KeywordSet() {
    static const std::unordered_set<std::string> s = {
        "alignas","alignof","and","asm","auto","break","case","catch","class","const",
        "constexpr","consteval","constinit","const_cast","continue","co_await","co_return",
        "co_yield","decltype","default","delete","do","dynamic_cast","else","enum","explicit",
        "export","extern","false","final","for","friend","goto","if","inline","int","long",
        "mutable","namespace","new","noexcept","nullptr","operator","or","override","private",
        "protected","public","register","reinterpret_cast","requires","return","short","signed",
        "sizeof","static","static_assert","static_cast","struct","switch","template","this",
        "thread_local","throw","true","try","typedef","typeid","typename","union","unsigned",
        "using","virtual","volatile","wchar_t","while","xor","and_eq","or_eq","not_eq","bitand",
        "bitor","compl","not","xor_eq","restrict"
    };
    return s;
}
bool IsKeywordName(const std::string& s) { return KeywordSet().count(s) > 0; }

// libstdc++ 内部命名约定: __xxx / _Xxx (下划线+大写) 一律视为内部名
bool IsNoiseName(const std::string& w) {
    if (w.empty()) return false;
    if (w.size() >= 2 && w[0] == '_' && (w[1] == '_' || isupper((unsigned char)w[1]))) return true;
    return false;
}

// 不能出现在"类型前缀"里的关键字 (控制/表达式关键字)
bool IsBadPrefixIdent(const std::string& s) {
    static const std::unordered_set<std::string> s2 = {
        "return","break","continue","goto","if","else","for","while","do","switch","case",
        "default","try","catch","throw","new","delete","sizeof","co_return","co_await",
        "co_yield","operator","true","false","nullptr","this","and","or","not","xor"
    };
    return s2.count(s) > 0;
}

// 声明前缀里应跳过的存储类/限定关键字 (不参与类型名)
bool IsSkipQualifier(const std::string& s) {
    static const std::unordered_set<std::string> q = {
        "const","volatile","static","extern","inline","register","mutable","constexpr",
        "consteval","constinit","thread_local","virtual","explicit","friend","typename",
        "struct","class","union","enum","restrict","__restrict"
    };
    return q.count(s) > 0;
}

// 表达式局部词法
struct ETok {
    enum K : uint8_t { Id, Num, Punct, Str, Char } k = Punct;
    std::string s;
};
void TokenizeExpr(const std::string& t, std::vector<ETok>& out) {
    size_t i = 0, n = t.size();
    auto push = [&](ETok::K k, size_t a, size_t b) { ETok e; e.k = k; e.s = t.substr(a, b - a); out.push_back(std::move(e)); };
    while (i < n) {
        char c = t[i];
        if (isspace((unsigned char)c)) { i++; continue; }
        if (c == '"' || c == '\'') { char q = c; size_t a = i; i++; while (i < n && t[i] != q) { if (t[i] == '\\' && i + 1 < n) i += 2; else i++; } if (i < n) i++; push(c == '"' ? ETok::Str : ETok::Char, a, i); continue; }
        if (IsIdStart(c)) { size_t a = i; while (i < n && IsIdChar(t[i])) i++; push(ETok::Id, a, i); continue; }
        if (isdigit((unsigned char)c)) { size_t a = i; while (i < n && (isalnum((unsigned char)t[i]) || t[i] == '.' || t[i] == '_')) i++; push(ETok::Num, a, i); continue; }
        if (i + 1 < n) { std::string p2 = t.substr(i, 2); if (p2 == "::" || p2 == "->") { push(ETok::Punct, i, i + 2); i += 2; continue; } }
        push(ETok::Punct, i, i + 1); i++;
    }
}

} // namespace

// ===================================================================
//  类型名归一化 (与 editor 中 NormTypeKey 语义一致)
// ===================================================================
std::string CodeModel::NormType(std::string t) {
    // 去空白
    std::string r; r.reserve(t.size());
    for (char c : t) if (!isspace((unsigned char)c)) r += c;
    // 去掉尾部的 [] (数组)
    while (!r.empty() && r.back() == ']') {
        size_t p = r.rfind('[');
        if (p == std::string::npos) break;
        r.erase(p);
    }
    // 去尾随 & * (含 &&)
    while (!r.empty() && (r.back() == '&' || r.back() == '*')) r.pop_back();
    // 去前置限定词 (长词优先, 按词边界)
    static const char* quals[] = {
        "constexpr","consteval","constinit","thread_local","const_cast","volatile","const",
        "static","extern","inline","register","mutable","virtual","explicit","friend",
        "typename","struct","class","union","enum","restrict","__restrict","signed","unsigned"
    };
    bool changed = true;
    while (changed && !r.empty()) {
        changed = false;
        for (auto q : quals) {
            size_t l = strlen(q);
            if (r.size() >= l && r.compare(0, l, q) == 0 && (r.size() == l || !IsIdChar(r[l]))) {
                r.erase(0, l); changed = true; break;
            }
        }
    }
    // 去 std::
    size_t p;
    while ((p = r.find("std::")) != std::string::npos) r.erase(p, 5);
    // 去模板参数 <...>
    size_t lt = r.find('<');
    if (lt != std::string::npos) r = r.substr(0, lt);
    // 前导/尾随 ::
    while (r.size() >= 2 && r.compare(0, 2, "::") == 0) r.erase(0, 2);
    while (r.size() >= 2 && r.compare(r.size() - 2, 2, "::") == 0) r.erase(r.size() - 2);
    return r;
}

// ===================================================================
//  词法分析
// ===================================================================
void CodeModel::Tokenize(const std::string& text) {
    size_t i = 0, n = text.size();
    int line = 0;
    auto push = [&](Tok::K k, size_t a, size_t b) {
        Tok t; t.k = k; t.s = text.substr(a, b - a); t.pos = a; t.end = b; t.line = line;
        m_toks.push_back(std::move(t));
    };
    while (i < n) {
        char c = text[i];
        if (c == '\n') { line++; i++; continue; }
        if (c == ' ' || c == '\t' || c == '\r') { i++; continue; }
        if (c == '/' && i + 1 < n && text[i + 1] == '/') {
            while (i < n && text[i] != '\n') i++;
            continue;
        }
        if (c == '/' && i + 1 < n && text[i + 1] == '*') {
            i += 2;
            while (i < n && !(text[i] == '*' && i + 1 < n && text[i + 1] == '/')) { if (text[i] == '\n') line++; i++; }
            if (i < n) i += 2;
            continue;
        }
        if (c == '#') {   // 预处理行: 整体跳过 (宏/包含不参与补全符号)
            while (i < n) {
                if (text[i] == '\n') {
                    if (i > 0 && text[i - 1] == '\\') { line++; i++; continue; }   // 续行
                    break;
                }
                i++;
            }
            continue;
        }
        if (c == '"' || c == '\'') {
            char q = c; size_t a = i; i++;
            while (i < n && text[i] != q) {
                if (text[i] == '\\' && i + 1 < n) i += 2;
                else { if (text[i] == '\n') line++; i++; }
            }
            if (i < n) i++;
            push(c == '"' ? Tok::Str : Tok::Char, a, i);
            continue;
        }
        if (IsIdStart(c)) { size_t a = i; while (i < n && IsIdChar(text[i])) i++; push(Tok::Ident, a, i); continue; }
        if (isdigit((unsigned char)c) || (c == '.' && i + 1 < n && isdigit((unsigned char)text[i + 1]))) {
            size_t a = i;
            while (i < n && (isalnum((unsigned char)text[i]) || text[i] == '.' || text[i] == '_')) i++;
            push(Tok::Number, a, i);
            continue;
        }
        // 双字符运算符 (不含 < >, 以免破坏模板解析)
        if (i + 1 < n) {
            std::string p2 = text.substr(i, 2);
            if (p2 == "::" || p2 == "->" || p2 == "++" || p2 == "--" || p2 == "==" || p2 == "!=" ||
                p2 == "<=" || p2 == ">=" || p2 == "&&" || p2 == "||" || p2 == "+=" || p2 == "-=" ||
                p2 == "*=" || p2 == "/=" || p2 == "%=" || p2 == "&=" || p2 == "|=" || p2 == "^=" || p2 == ".*") {
                push(Tok::Punct, i, i + 2); i += 2; continue;
            }
        }
        push(Tok::Punct, i, i + 1); i++;
    }
}

// ===================================================================
//  主解析
// ===================================================================
void CodeModel::Rebuild(const std::string& text) {
    m_text = text;
    m_toks.clear();
    m_globals.clear(); m_declarationSites.clear();
    m_locals.clear();
    m_types.clear();
    m_aliases.clear();
    m_aliasPos.clear();
    m_knownTypes.clear();
    m_namespaces.clear();
    m_classScopes.clear();
    m_funcScopes.clear();
    m_namespaceScopes.clear(); m_usingImports.clear(); m_inlineNamespaces.clear();
    Tokenize(text);
    for (auto& b : BuiltinSet()) m_knownTypes.insert(b);
    m_valid = true;
    m_hash = std::hash<std::string>{}(text);

    const size_t N = m_toks.size();
    if (N == 0) return;

    struct Frame {
        enum K { Global, Block, Class, Func, Namespace } k = Global;
        size_t openPos = 0;
        std::string key;        // Class: m_types 键
        std::string cls;        // Class: 显示名; Func: this 类型
        std::string ns;         // Namespace: 命名空间限定名
        size_t localStart = 0;
        int    access = 0;
        bool   typedefDef = false;
        bool   anon = false;
        size_t nsIndex = size_t(-1);
    };
    std::vector<Frame> st;
    { Frame g; g.k = Frame::Global; g.localStart = 0; st.push_back(g); }

    struct PendClass { bool active = false; std::string key, name; bool anon = false; bool typedefDef = false; bool isStruct = false; };
    struct PendFunc  { bool active = false; std::string thisType; std::vector<CSym> params; };
    PendClass pendClass;
    PendFunc  pendFunc;
    bool pendNamespace = false;
    bool pendInlineNamespace = false;
    std::string pendNsName;
    bool pendTypedef = false;
    int  anonCounter = 0;

    auto tokStr = [&](size_t i) -> const std::string& { static const std::string e; return i < N ? m_toks[i].s : e; };

    auto MatchParen = [&](size_t i) -> size_t {
        int d = 0;
        for (size_t k = i; k < N; k++) {
            if (tokStr(k) == "(") d++;
            else if (tokStr(k) == ")") { d--; if (d == 0) return k + 1; }
        }
        return NPOS;
    };
    auto MatchBrace = [&](size_t i) -> size_t {
        int d = 0;
        for (size_t k = i; k < N; k++) {
            if (tokStr(k) == "{") d++;
            else if (tokStr(k) == "}") { d--; if (d == 0) return k + 1; }
        }
        return NPOS;
    };
    auto MatchBracket = [&](size_t i) -> size_t {
        int d = 0;
        for (size_t k = i; k < N; k++) {
            if (tokStr(k) == "[") d++;
            else if (tokStr(k) == "]") { d--; if (d == 0) return k + 1; }
        }
        return NPOS;
    };
    // 跳过模板参数 <...> (仅用于 类名后 判断特化定义, 如 struct char_traits<char>)
    auto MatchAngle = [&](size_t i) -> size_t {
        auto Adjacent = [&](size_t a, size_t b2, const char* op) {
            return b2 < N && tokStr(b2) == op && m_toks[a].end == m_toks[b2].pos;
        };
        int d = 0;
        for (size_t k = i; k < N; k++) {
            const std::string& s = tokStr(k);
            if (s == "<") { if (Adjacent(k, k + 1, "<")) { k++; continue; } d++; }
            else if (s == ">") {
                if (Adjacent(k, k + 1, ">")) { d -= 2; k++; if (d <= 0) return k + 1; continue; }
                d--; if (d == 0) return k + 1;
            }
            else if (s == ";" || s == "{" || s == "}") return NPOS;
        }
        return NPOS;
    };
    auto SkipStatement = [&](size_t i) -> size_t {
        int dep = 0;
        for (size_t k = i; k < N; k++) {
            const std::string& s = m_toks[k].s;
            if (s == "(" || s == "[") dep++;
            else if (s == ")" || s == "]") { if (dep > 0) dep--; }
            else if (dep == 0) {
                if (s == ";") return k + 1;
                if (s == "{" || s == "}") return k;
            }
        }
        return N;
    };
    auto SkipInitializer = [&](size_t i) -> size_t {
        int dep = 0;
        for (size_t k = i; k < N; k++) {
            const std::string& s = m_toks[k].s;
            if (s == "(" || s == "[" || s == "{") dep++;
            else if (s == ")" || s == "]" || s == "}") { if (dep > 0) dep--; }
            else if (dep == 0 && (s == "," || s == ";")) return k;
        }
        return N;
    };
    auto CurClass = [&]() -> std::string {
        for (int k = (int)st.size() - 1; k >= 0; k--)
            if (st[k].k == Frame::Class) return st[k].cls.empty() ? st[k].key : st[k].cls;
        return "";
    };
    auto CurNs = [&]() -> std::string {
        for (int k = (int)st.size() - 1; k >= 0; k--)
            if (st[k].k == Frame::Namespace) return st[k].ns;
        return "";
    };
    auto InClassBody = [&]() { return !st.empty() && st.back().k == Frame::Class; };

    auto AddNamespace = [&](const std::string& name, size_t pos) {
        std::string parent = CurNs();
        size_t begin = 0;
        while (begin < name.size()) {
            size_t sep = name.find("::", begin);
            std::string part = name.substr(begin, sep == std::string::npos ? sep : sep - begin);
            std::string full = parent.empty() ? part : parent + "::" + part;
            CSym declaration; declaration.name=part; declaration.ns=parent;
            declaration.kind=CSymKind::Namespace; declaration.declPos=pos; declaration.defined=true;
            m_declarationSites.push_back(declaration);
            if (m_namespaces.insert(full).second) {
                CSym symbol;
                symbol.name = part; symbol.ns = parent; symbol.kind = CSymKind::Namespace;
                symbol.declPos = pos; symbol.defined = true;
                m_globals.push_back(std::move(symbol));
            }
            parent = full;
            if (sep == std::string::npos) break;
            begin = sep + 2;
        }
    };

    // 记录一个类型名符号 (供词/命名空间补全). isDef=false 表示前置声明 (class X;)
    // 同名符号: 已有"定义体"则不再被前置声明覆盖; 先前的"前置声明"可被后续"定义体"替换
    // (libstdc++ 的 iosfwd/stringfwd.h 等前置声明头在翻译单元里出现更早, 必须让真正的
    //  类型定义胜出, 否则跳转定义会落到 iosfwd 这样的前置声明上)。
    auto AddTypeSym = [&](const std::string& nm, CSymKind kind, size_t pos, bool isDef = true) {
        if (nm.empty() || nm[0] == '$') return;
        if (m_system && IsNoiseName(nm)) return;
        CSym declaration; declaration.name=nm; declaration.ns=CurNs(); declaration.kind=kind;
        declaration.declPos=pos; declaration.defined=isDef; m_declarationSites.push_back(declaration);
        for (auto& g : m_globals) {
            if (g.kind == kind && g.name == nm) {
                if (isDef && !g.defined) { g.declPos = pos; g.ns = CurNs(); g.defined = true; }
                return;
            }
        }
        CSym s; s.name = nm; s.type = nm; s.kind = kind; s.declPos = pos; s.ns = CurNs();
        s.defined = isDef;
        m_globals.push_back(std::move(s));
    };

    // 注册变量/成员
    auto AddVar = [&](const std::string& vname, const std::string& typRaw, const std::string& init, size_t declPos) {
        if (vname.empty() || IsKeywordName(vname)) return;
        if (m_system && IsNoiseName(vname)) return;
        std::string typ = CleanType(Trim(typRaw));
        if (InClassBody()) {
            CMember cm; cm.name = vname; cm.type = typ; cm.kind = CSymKind::Member;
            cm.access = st.back().access; cm.declPos = declPos;
            auto it = m_types.find(st.back().key);
            if (it != m_types.end()) {
                for (auto& e : it->second.members) if (e.name == vname) return;
                it->second.members.push_back(std::move(cm));
            }
            return;
        }
        bool global = st.empty() || st.back().k == Frame::Global || st.back().k == Frame::Namespace;
        CSym s; s.name = vname; s.type = typ; s.kind = CSymKind::Variable; s.declPos = declPos; s.initExpr = init;
        if (global) {
            s.ns = CurNs();
            m_globals.push_back(std::move(s));
        } else {
            s.isLocal = true; s.scopeStart = st.back().openPos; s.scopeEnd = 0;
            m_locals.push_back(std::move(s));
        }
    };

    // 解析一串声明符 (type name1, name2 ...)
    auto ParseVarList = [&](const std::string& typeRaw, size_t startName) -> size_t {
        size_t cur = startName;
        while (cur < N) {
            if (m_toks[cur].k != Tok::Ident) return cur;
            std::string vname = m_toks[cur].s;
            if (IsKeywordName(vname)) return cur;
            size_t m = cur + 1;
            while (m < N && tokStr(m) == "[") { size_t e = MatchBracket(m); if (e == NPOS) return cur + 1; m = e; }
            std::string initExpr;
            if (tokStr(m) == "=") {
                size_t e = SkipInitializer(m + 1);
                if (m + 2 <= e && e <= N) {   // 跳过 '=' 本身, 只保留初始化表达式
                    size_t st = m_toks[m + 1].pos;
                    size_t en = (e < N) ? m_toks[e].pos : m_text.size();
                    if (en > st) initExpr = m_text.substr(st, en - st);
                }
                m = e;
            } else if (tokStr(m) == "{") {
                size_t e = MatchBrace(m); if (e == NPOS) return cur + 1; m = e;
            }
            AddVar(vname, typeRaw, initExpr, m_toks[cur].pos);
            if (tokStr(m) == ",") {
                size_t nk = m + 1;
                while (nk < N && (tokStr(nk) == "*" || tokStr(nk) == "&")) nk++;
                if (nk < N && m_toks[nk].k == Tok::Ident) { cur = nk; continue; }
                return m + 1;
            }
            if (tokStr(m) == ";") return m + 1;
            return m;
        }
        return cur;
    };

    // 解析函数参数 -> 参数符号
    auto ParseParams = [&](size_t open, size_t close) -> std::vector<CSym> {
        std::vector<CSym> res;
        if (close <= open + 1) return res;
        size_t a = open + 1, b = close - 1;
        size_t seg = a; int dep = 0;
        for (size_t x = a; x <= b; x++) {
            bool atEnd = (x == b);
            if (!atEnd) {
                const std::string& s = tokStr(x);
                if (s == "(" || s == "[" || s == "{" || s == "<") dep++;
                else if (s == ")" || s == "]" || s == "}" || s == ">") { if (dep > 0) dep--; }
            }
            if (atEnd || (dep == 0 && tokStr(x) == ",")) {
                size_t segEnd = x;
                size_t eq = NPOS;
                for (size_t y = seg; y < segEnd; y++) if (tokStr(y) == "=") { eq = y; break; }
                size_t realEnd = (eq == NPOS) ? segEnd : eq;
                size_t nmTok = NPOS;
                for (size_t y = seg; y < realEnd; y++)
                    if (m_toks[y].k == Tok::Ident && !IsKeywordName(m_toks[y].s)) nmTok = y;
                if (nmTok != NPOS && nmTok > seg) {
                    std::string t;
                    for (size_t y = seg; y < nmTok; y++) {
                        const std::string& s = tokStr(y);
                        if (m_toks[y].k == Tok::Ident) { if (!IsSkipQualifier(s)) { t += s; t += ' '; } }
                        else if (s == "::" || s == "<" || s == ">" || s == "*" || s == "&") t += s;
                        else if (s == ",") t += ", ";
                    }
                    CSym s; s.name = m_toks[nmTok].s; s.kind = CSymKind::Parameter; s.type = CleanType(Trim(t));
                    res.push_back(std::move(s));
                }
                seg = x + 1;
            }
        }
        return res;
    };

    // 注册函数/方法; 返回新索引
    auto ParseFuncDecl = [&](size_t i, size_t nameTok, const std::string& typeRaw,
                             const std::string& qualClass, bool qualified, bool ctor) -> size_t {
        size_t open = nameTok + 1;
        size_t close = MatchParen(open);
        if (close == NPOS) return SkipStatement(i);
        size_t q = close;
        while (q < N && (tokStr(q) == "const" || tokStr(q) == "volatile" || tokStr(q) == "noexcept" ||
                         tokStr(q) == "override" || tokStr(q) == "final" || tokStr(q) == "&" || tokStr(q) == "&&")) q++;
        if (tokStr(q) == "->") {
            size_t r = q + 1;
            while (r < N && tokStr(r) != "{" && tokStr(r) != ";" && tokStr(r) != "=") r++;
            q = r;
        }
        bool isDef = false, ok = false;
        if (tokStr(q) == "{") { isDef = true; ok = true; }
        else if (tokStr(q) == ";") { ok = true; }
        else if (tokStr(q) == "=") { ok = true; size_t r = q + 1; while (r < N && tokStr(r) != ";") r++; q = r; }
        else if (tokStr(q) == ":") {
            size_t r = q + 1;
            while (r < N) {
                const std::string& s = tokStr(r);
                if (s == "{" || s == ";") break;
                if (s == "(") { size_t e = MatchParen(r); if (e == NPOS) break; r = e; continue; }
                r++;
            }
            if (tokStr(r) == "{") { q = r; ok = true; isDef = true; }
        }
        if (!ok) return i;

        std::string name = m_toks[nameTok].s;
        // 签名: name(原始参数文本)
        std::string args = m_text.substr(m_toks[open].pos, m_toks[close - 1].end - m_toks[open].pos);
        std::string sig = name + Collapse(args);
        if (sig.size() > 120) sig = sig.substr(0, 117) + "...";

        bool member = false; std::string clsKey; int access = 0; std::string thisType;
        if (!qualified && InClassBody()) {
            member = true; clsKey = st.back().key; access = st.back().access; thisType = st.back().cls;
        } else if (qualified) {
            member = true; clsKey = qualClass; access = 0; thisType = qualClass;
        }
        std::string ret = ctor ? (qualified ? qualClass : CurClass()) : CleanType(Trim(typeRaw));

        bool skipReg = m_system && IsNoiseName(name);
        if (member) {
            if (!skipReg) {
                CMember cm; cm.name = name; cm.type = ret; cm.sig = sig;
                 cm.kind = CSymKind::Function; cm.access = access; cm.declPos = m_toks[i].pos;
                auto it = m_types.find(clsKey);
                if (it == m_types.end()) { CTypeInfo ci; ci.name = clsKey; ci.ok = true; m_types[clsKey] = ci; it = m_types.find(clsKey); }
                bool dup = false;
                for (auto& e : it->second.members) if (e.name == name && e.sig == sig) { dup = true; break; }
                if (!dup) it->second.members.push_back(std::move(cm));
                m_knownTypes.insert(clsKey);
            }
        } else if (!skipReg) {
            CSym fs; fs.name = name; fs.type = ret; fs.sig = sig; fs.kind = CSymKind::Function;
            fs.declPos = m_toks[i].pos; fs.ns = CurNs();
            m_globals.push_back(std::move(fs));
        }

        if (isDef) {
            pendFunc.active = true;
            pendFunc.thisType = member ? thisType : "";
            pendFunc.params = ParseParams(open, close);
            return q;   // 交给主循环遇 '{' 建函数体帧
        }
        return (q < N) ? q + 1 : q;
    };

    // 变量声明列表 (供 struct Foo var; 复用)
    // class / struct / union
    auto ParseClassKeyword = [&](size_t i) -> size_t {
        std::string kw = tokStr(i);
        bool isStruct = (kw == "struct");
        bool typedefDef = pendTypedef; pendTypedef = false;
        size_t k = i + 1;
        std::string nm; bool anon = false;
        if (k < N && m_toks[k].k == Tok::Ident && !IsKeywordName(m_toks[k].s)) { nm = m_toks[k].s; k++; }
        else anon = true;
        if (tokStr(k) == "final") k++;
        // 模板特化: 跳过类名后的 <...> (如 struct char_traits<char> { ... })
        if (tokStr(k) == "<") { size_t e = MatchAngle(k); if (e != NPOS && e < N) k = e; }
        if (tokStr(k) == ";") {   // 前置声明 (class X;)
            if (!nm.empty()) {
                m_knownTypes.insert(nm);
                if (!m_types.count(nm)) { CTypeInfo ci; ci.name = nm; ci.ok = true; ci.isStruct = isStruct; m_types[nm] = ci; }
                AddTypeSym(nm, CSymKind::Type, m_toks[i].pos, false);
            }
            return k + 1;
        }
        // 基类
        std::vector<std::string> bases;
        if (tokStr(k) == ":") {
            k++;
            std::string cur;
            while (k < N && tokStr(k) != "{") {
                const std::string& t = tokStr(k);
                if (t == ",") { if (!cur.empty()) { bases.push_back(NormType(cur)); cur.clear(); } }
                else if (t == ";") break;
                else if (t == "public" || t == "private" || t == "protected" || t == "virtual") { /* 跳过 */ }
                else if (m_toks[k].k == Tok::Ident || t == "::" || t == "<" || t == ">") cur += t;
                k++;
            }
            if (!cur.empty()) bases.push_back(NormType(cur));
        }
        if (tokStr(k) != "{") {
            // struct Foo var; 形式
            if (!nm.empty() && k < N && m_toks[k].k == Tok::Ident) {
                return ParseVarList(nm, k);
            }
            return (k > i) ? k : i + 1;
        }
        std::string key = anon ? ("$anon" + std::to_string(++anonCounter)) : nm;
        auto cit = m_types.find(key);
        if (cit == m_types.end()) {
            CTypeInfo ci; ci.name = nm; ci.ok = true; ci.isStruct = isStruct; ci.bases = bases;
            m_types[key] = ci;
        } else {                       // 多次定义 (条件编译分支): 合并成员
            cit->second.ok = true;
            cit->second.name = nm;
            cit->second.isStruct = isStruct;
            if (!bases.empty()) cit->second.bases = bases;
        }
        if (!nm.empty()) { m_knownTypes.insert(nm); AddTypeSym(nm, CSymKind::Type, m_toks[i].pos); }
        pendClass.active = true; pendClass.key = key; pendClass.name = nm;
        pendClass.anon = anon; pendClass.typedefDef = typedefDef; pendClass.isStruct = isStruct;
        return k;   // 指向 '{'
    };

    // enum [class] Name [: type] { ... }  /  enum Name;
    auto ParseEnum = [&](size_t i) -> size_t {
        bool typedefDef = pendTypedef; pendTypedef = false;
        bool scoped = (tokStr(i + 1) == "class" || tokStr(i + 1) == "struct");
        size_t k = i + 1;
        if (scoped) k++;
        std::string nm; bool anon = false;
        if (k < N && m_toks[k].k == Tok::Ident && !IsKeywordName(m_toks[k].s)) { nm = m_toks[k].s; k++; }
        else anon = true;
        if (tokStr(k) == ":") { while (k < N && tokStr(k) != "{") k++; }
        if (tokStr(k) != "{") return (k > i) ? k : i + 1;
        std::string key = anon ? ("$anon" + std::to_string(++anonCounter)) : nm;
        auto eit = m_types.find(key);
        if (eit == m_types.end()) { CTypeInfo ci; ci.name = nm; ci.ok = true; ci.isEnum = true; m_types[key] = ci; }
        else { eit->second.ok = true; eit->second.name = nm; eit->second.isEnum = true; }
        if (!nm.empty()) { m_knownTypes.insert(nm); AddTypeSym(nm, CSymKind::Enum, m_toks[i].pos); }
        size_t j = k + 1; int depth = 1;
        bool expectName = true;
        while (j < N && depth > 0) {
            const std::string& s = tokStr(j);
            if (s == "{") { depth++; expectName = true; j++; continue; }
            if (s == "}") { depth--; j++; if (depth == 0) break; continue; }
            if (depth == 1 && s == ",") { expectName = true; j++; continue; }
            if (depth == 1 && m_toks[j].k == Tok::Ident && expectName) {
                CMember cm; cm.name = s; cm.kind = CSymKind::Enumerator; cm.type = nm.empty() ? key : nm;
                cm.declPos = m_toks[j].pos;
                m_types[key].members.push_back(cm);
                // 类内的非作用域枚举: 枚举值也可通过 类名::值 访问 (如 ios_base::in / ios_base::out)
                if (!scoped && InClassBody()) {
                    auto cit2 = m_types.find(st.back().key);
                    if (cit2 != m_types.end()) {
                        bool dupE = false;
                        for (auto& e : cit2->second.members) if (e.name == s) { dupE = true; break; }
                        if (!dupE) cit2->second.members.push_back(cm);
                    }
                }
                if (!scoped) {   // 非作用域枚举: 值进入全局符号
                    CSym g; g.name = s; g.type = cm.type; g.kind = CSymKind::Enumerator;
                    g.declPos = m_toks[j].pos; g.ns = CurNs();
                    m_globals.push_back(std::move(g));
                }
                expectName = false;
            }
            j++;
        }
        if (typedefDef) {
            std::string firstAlias;
            size_t x = j;
            while (x < N && tokStr(x) != ";") {
                if (m_toks[x].k == Tok::Ident && !IsKeywordName(m_toks[x].s)) {
                    if (firstAlias.empty()) firstAlias = m_toks[x].s;
                    m_aliases[m_toks[x].s] = key; m_knownTypes.insert(m_toks[x].s);
                }
                x++;
            }
            if (anon && !firstAlias.empty()) {
                auto it = m_types.find(key);
                if (it != m_types.end()) { CTypeInfo c2 = it->second; c2.name = firstAlias; m_types[firstAlias] = c2; m_types.erase(it); }
            }
            j = (x < N) ? x + 1 : x;
        }
        return j;
    };

    // using ...
    auto ParseUsing = [&](size_t i) -> size_t {
        size_t k = i + 1;
        if (tokStr(k) == "enum") return SkipStatement(k);
        bool namespc = tokStr(k) == "namespace";
        if(namespc) ++k;
        // using namespace N; and using N::name; are visibility imports, not types.
        size_t end = SkipStatement(k);
        if(namespc || tokStr(k)=="::" || tokStr(k+1)=="::") {
            UsingImport import{}; import.namespc=namespc; import.owner=CurNs();
            import.declPos=m_toks[i].pos; import.start=st.back().openPos;
            import.end=m_text.size(); import.local=st.back().k!=Frame::Global && st.back().k!=Frame::Namespace;
            import.absolute=tokStr(k)=="::";
            for(size_t x=k;x<end && tokStr(x)!=";";++x)
                if(m_toks[x].k==Tok::Ident || tokStr(x)=="::") import.target+=tokStr(x);
            if(import.target.compare(0,2,"::")==0) import.target.erase(0,2);
            if(!import.target.empty()) m_usingImports.push_back(std::move(import));
            return end;
        }
        if (k < N && m_toks[k].k == Tok::Ident) {
            std::string nm = m_toks[k].s;
            size_t k2 = k + 1;
            if (tokStr(k2) == "=") {
                size_t e = SkipStatement(k2);
                std::string t;
                for (size_t x = k2 + 1; x < e && tokStr(x) != ";"; x++) {
                    const std::string& s = tokStr(x);
                    if (m_toks[x].k == Tok::Ident) { if (!IsSkipQualifier(s)) { t += s; t += ' '; } }
                    else if (s == "::" || s == "<" || s == ">" || s == "*" || s == "&") t += s;
                    else if (s == ",") t += ", ";
                }
                m_aliases[nm] = NormType(t);
                m_aliasPos[nm] = m_toks[k].pos;
                m_knownTypes.insert(nm);
                if (!(m_system && IsNoiseName(nm))) AddTypeSym(nm, CSymKind::Type, m_toks[k].pos);
                return e;
            }
        }
        return SkipStatement(k);
    };

    // typedef ...
    auto ParseTypedef = [&](size_t i) -> size_t {
        size_t k = i + 1;
        if (tokStr(k) == "struct" || tokStr(k) == "class" || tokStr(k) == "union" || tokStr(k) == "enum") {
            // 探测 '{' 是否在同一语句内
            size_t semi = NPOS, brace = NPOS;
            for (size_t x = k + 1; x < N; x++) {
                if (tokStr(x) == ";") { semi = x; break; }
                if (tokStr(x) == "{") { brace = x; break; }
            }
            if (brace != NPOS && (semi == NPOS || brace < semi)) {
                pendTypedef = true;      // 交给 class/struct/enum 分支处理, 结束后读别名
                return k;
            }
        }
        // 简单别名: typedef T NAME[, NAME2];
        size_t e = SkipStatement(k);
        size_t semi = (e > 0 && e <= N && tokStr(e - 1) == ";") ? e - 1 : e;
        size_t segStart = k;
        std::string lastType;
        bool firstSeg = true;
        for (size_t x = k; x <= semi; x++) {
            if (x == semi || tokStr(x) == ",") {
                size_t aliasTok = NPOS;
                for (size_t y = segStart; y < x; y++)
                    if (m_toks[y].k == Tok::Ident && !IsKeywordName(m_toks[y].s)) aliasTok = y;
                if (aliasTok != NPOS) {
                    std::string alias = m_toks[aliasTok].s;
                    if (firstSeg) {
                        std::string t;
                        for (size_t y = segStart; y < aliasTok; y++) {
                            const std::string& s = tokStr(y);
                            if (m_toks[y].k == Tok::Ident) { if (!IsSkipQualifier(s)) { t += s; t += ' '; } }
                            else if (s == "::" || s == "<" || s == ">" || s == "*" || s == "&") t += s;
                        }
                        lastType = NormType(t);
                        firstSeg = false;
                    }
                    m_aliases[alias] = lastType;
                    m_aliasPos[alias] = m_toks[aliasTok].pos;
                    m_knownTypes.insert(alias);
                    if (!(m_system && IsNoiseName(alias))) AddTypeSym(alias, CSymKind::Type, m_toks[aliasTok].pos);
                }
                segStart = x + 1;
            }
        }
        return (e > i) ? e : i + 1;
    };

    // for (...) 头部内的声明 (循环变量)
    auto ParseForInit = [&](size_t start, size_t end, bool isRange) {
        (void)isRange;
        if (start >= end) return;
        // 按顶层逗号分段
        size_t seg = start; int dep = 0;
        for (size_t x = start; x <= end; x++) {
            bool atEnd = (x == end);
            if (!atEnd) {
                const std::string& s = tokStr(x);
                if (s == "(" || s == "[" || s == "{" || s == "<") dep++;
                else if (s == ")" || s == "]" || s == "}" || s == ">") { if (dep > 0) dep--; }
            }
            if (atEnd || (dep == 0 && tokStr(x) == ",")) {
                size_t segEnd = x;
                size_t stop = segEnd;
                for (size_t y = seg; y < segEnd; y++) if (tokStr(y) == "=" || tokStr(y) == ":") { stop = y; break; }
                size_t nmTok = NPOS;
                for (size_t y = seg; y < stop; y++)
                    if (m_toks[y].k == Tok::Ident && !IsKeywordName(m_toks[y].s)) nmTok = y;
                if (nmTok != NPOS && nmTok > seg) {
                    std::string t;
                    for (size_t y = seg; y < nmTok; y++) {
                        const std::string& s = tokStr(y);
                        if (m_toks[y].k == Tok::Ident) { if (!IsSkipQualifier(s)) { t += s; t += ' '; } }
                        else if (s == "::" || s == "<" || s == ">" || s == "*" || s == "&") t += s;
                    }
                    CSym s; s.name = m_toks[nmTok].s; s.kind = CSymKind::Variable; s.type = CleanType(Trim(t));
                    s.declPos = m_toks[nmTok].pos; s.isLocal = true;
                    s.scopeStart = !st.empty() ? st.back().openPos : 0; s.scopeEnd = 0;
                    m_locals.push_back(std::move(s));
                }
                seg = x + 1;
            }
        }
    };

    // 尝试把一个语句解析为声明; 失败返回原索引
    auto TryDecl = [&](size_t i) -> size_t {
        if (i >= N || m_toks[i].k != Tok::Ident) return i;
        const std::string& f0 = m_toks[i].s;
        if (IsBadPrefixIdent(f0)) return i;
        int angle = 0; size_t nameTok = NPOS;
        size_t k = i;
        for (; k < N; k++) {
            const std::string& s = m_toks[k].s;
            if (s == "," && angle > 0) continue; // e.g. basic_ostream<Char, Traits>* tie()
            if (s == "(" || s == ";" || s == "=" || s == "," || s == "[" || s == "{" || s == ":") break;
            if (s == "<") { angle++; continue; }
            if (s == ">") { if (angle > 0) angle--; continue; }
            if (s == "." || s == "->") return i;
            if (m_toks[k].k == Tok::Ident) { if (IsBadPrefixIdent(s)) return i; nameTok = k; continue; }
            if (s == "*" || s == "&" || s == "::" || s == "~") continue;
            return i;
        }
        if (angle != 0) return i;
        if (nameTok == NPOS || nameTok == i) return i;
        std::string name = m_toks[nameTok].s;
        if (IsKeywordName(name)) return i;

        std::string typeStr; bool hasTypeTok = false;
        for (size_t t = i; t < nameTok; t++) {
            const std::string& s = m_toks[t].s;
            if (m_toks[t].k == Tok::Ident) {
                if (IsBadPrefixIdent(s)) return i;
                if (IsSkipQualifier(s)) continue;
                if (BuiltinSet().count(s) || m_knownTypes.count(s)) hasTypeTok = true;
                typeStr += s; typeStr += ' ';
            } else if (s == "::" || s == "<") { typeStr += s; hasTypeTok = true; }
            else if (s == ">" || s == "*" || s == "&") { typeStr += s; }
            else if (s == ",") { typeStr += ", "; }
            else return i;
        }

        bool qualified = false; std::string qualClass;
        if (nameTok >= 1 && m_toks[nameTok - 1].s == "::") {
            qualified = true;
            size_t q = nameTok - 1;
            if (q >= 1 && m_toks[q - 1].k == Tok::Ident) qualClass = m_toks[q - 1].s;
            else return i;
            bool hasRet = false;
            for (size_t t = i; t + 1 < q; t++)
                if (m_toks[t].k == Tok::Ident && (BuiltinSet().count(m_toks[t].s) || m_knownTypes.count(m_toks[t].s))) hasRet = true;
            if (!hasRet) return i;
        }

        std::string cc = CurClass();
        bool ctor = (!cc.empty() && name == cc) || (qualified && name == qualClass);
        if (!hasTypeTok && !ctor) return i;

        size_t nx = nameTok + 1;
        if (nx >= N) return i;
        const std::string& nxS = m_toks[nx].s;
        if (nxS == "(") return ParseFuncDecl(i, nameTok, typeStr, qualClass, qualified, ctor);
        if (nxS == ";" || nxS == "=" || nxS == "," || nxS == "[" || nxS == "{") {
            size_t e = ParseVarList(typeStr, nameTok);
            return (e > i) ? e : i;
        }
        return i;
    };

    // ---------------- 主循环 ----------------
    size_t i = 0;
    while (i < N) {
        const std::string& s = m_toks[i].s;
        if (s == "{") {
            Frame f; f.openPos = m_toks[i].pos; f.localStart = m_locals.size();
            if (pendFunc.active) {
                f.k = Frame::Func; f.cls = pendFunc.thisType;
                for (auto& p : pendFunc.params) { CSym ps = p; ps.isLocal = true; ps.scopeStart = f.openPos; ps.scopeEnd = 0; m_locals.push_back(std::move(ps)); }
                pendFunc.active = false;
            } else if (pendClass.active) {
                f.k = Frame::Class; f.key = pendClass.key; f.cls = pendClass.name;
                f.typedefDef = pendClass.typedefDef; f.anon = pendClass.anon; f.access = 0;
                pendClass.active = false;
            } else if (pendNamespace) {
                f.k = Frame::Namespace;
                std::string parent = CurNs();
                f.ns = pendNsName.empty() ? parent
                     : (parent.empty() ? pendNsName : parent + "::" + pendNsName);
                f.nsIndex=m_namespaceScopes.size();
                m_namespaceScopes.push_back({f.openPos,m_text.size(),f.ns});
                if(pendInlineNamespace) m_inlineNamespaces.insert(f.ns);
                pendNamespace = false; pendInlineNamespace=false;
            } else {
                f.k = Frame::Block;
            }
            st.push_back(f);
            i++; continue;
        }
        if (s == "}") {
            if (st.size() > 1) {
                Frame f = st.back(); st.pop_back();
                size_t endPos = m_toks[i].pos;
                if(f.nsIndex!=size_t(-1)) m_namespaceScopes[f.nsIndex].end=endPos;
                for(auto& import:m_usingImports) if(import.local && import.start==f.openPos) import.end=endPos;
                if (f.k == Frame::Func) m_funcScopes.push_back({ f.openPos, endPos, f.cls });
                if (f.k == Frame::Class) {
                    m_classScopes.push_back({ f.openPos, endPos, f.cls.empty() ? f.key : f.cls });
                    if (f.typedefDef) {
                        std::string firstAlias;
                        size_t x = i + 1;
                        while (x < N && tokStr(x) != ";") {
                            if (m_toks[x].k == Tok::Ident && !IsKeywordName(m_toks[x].s)) {
                                if (firstAlias.empty()) firstAlias = m_toks[x].s;
                                m_aliases[m_toks[x].s] = f.key;
                                m_aliasPos[m_toks[x].s] = m_toks[x].pos;
                                m_knownTypes.insert(m_toks[x].s);
                                if (!(m_system && IsNoiseName(m_toks[x].s))) AddTypeSym(m_toks[x].s, CSymKind::Type, m_toks[x].pos);
                            }
                            x++;
                        }
                        if (f.anon && !firstAlias.empty()) {
                            auto it = m_types.find(f.key);
                            if (it != m_types.end()) { CTypeInfo c2 = it->second; c2.name = firstAlias; m_types[firstAlias] = c2; m_types.erase(it); }
                        }
                    }
                }
                for (size_t k2 = f.localStart; k2 < m_locals.size(); k2++)
                    if (m_locals[k2].scopeEnd == 0) m_locals[k2].scopeEnd = endPos;
            }
            i++; continue;
        }
        if (s == ";") { i++; continue; }
        // 类内访问修饰符
        if (InClassBody() && (s == "public" || s == "private" || s == "protected") && tokStr(i + 1) == ":") {
            st.back().access = (s == "public") ? 0 : (s == "protected" ? 1 : 2);
            i += 2; continue;
        }
        if (s == "template") {
            size_t k = i + 1;
            // 跳过模板参数表 <...>。注意: '<' '>' 也可能来自移位运算符 "<<"/">>" (如模板
            // 默认实参里的 decltype(a << b))。tokenizer 把 '<<' 拆成两个相邻 '<', 若不加
            // 区分会把嵌套深度数错, 一路吞掉后面的声明 (曾导致整个 istream 类被跳过)。
            if (tokStr(k) == "<") {
                auto Adjacent = [&](size_t a, size_t b2, const char* op) {
                    return b2 < N && tokStr(b2) == op && m_toks[a].end == m_toks[b2].pos;
                };
                int ang = 0;
                while (k < N) {
                    const std::string& t = tokStr(k);
                    if (t == "<") {
                        if (Adjacent(k, k + 1, "<")) { k += 2; continue; }   // '<<' 移位
                        ang++; k++; continue;
                    }
                    if (t == ">") {
                        if (Adjacent(k, k + 1, ">")) { ang -= 2; k++; }      // '>>' 收两层
                        else ang--;
                        k++;
                        if (ang <= 0) break;
                        continue;
                    }
                    if (m_toks[k].k == Tok::Ident && t != "typename" && t != "class" && !IsKeywordName(t)) m_knownTypes.insert(t);
                    k++;
                }
            }
            i = k; continue;
        }
        if (s == "inline" && tokStr(i+1)=="namespace") { ++i; continue; }
        if (s == "namespace") {
            size_t k = i + 1;
            if (tokStr(k) == "inline") k++;
            // 命名空间名: 只吃一个标识符 (可含 A::B), 不要吞掉后面的宏/属性
            std::string nm;
            if (k < N && m_toks[k].k == Tok::Ident) {
                nm = m_toks[k].s; k++;
                while (k + 1 < N && tokStr(k) == "::" && m_toks[k + 1].k == Tok::Ident) {
                    nm += "::"; nm += m_toks[k + 1].s; k += 2;
                }
            }
            if (tokStr(k) == "=") { size_t e = SkipStatement(k); i = (e > k) ? e : k + 1; continue; }
            // 跳过 attribute/可见性宏: NAME ( ... )  (如 _GLIBCXX_VISIBILITY(default))
            while (k + 1 < N && m_toks[k].k == Tok::Ident && tokStr(k + 1) == "(") {
                size_t e = MatchParen(k + 1);
                if (e == NPOS) break;
                k = e;
            }
            if (tokStr(k) == "{") { AddNamespace(nm, m_toks[i].pos); pendNamespace = true; pendInlineNamespace = i>0 && tokStr(i-1)=="inline"; pendNsName = nm; i = k; continue; }
            i = (k > i) ? k : i + 1; continue;
        }
        if (s == "class" || s == "struct" || s == "union") { i = ParseClassKeyword(i); continue; }
        if (s == "enum") { i = ParseEnum(i); continue; }
        if (s == "using") { i = ParseUsing(i); continue; }
        if (s == "typedef") { i = ParseTypedef(i); continue; }
        if (s == "for") {
            size_t k = i + 1;
            if (tokStr(k) != "(") { size_t e = SkipStatement(i); i = (e > i) ? e : i + 1; continue; }
            size_t close = MatchParen(k);
            if (close == NPOS) { size_t e = SkipStatement(i); i = (e > i) ? e : i + 1; continue; }
            // 找顶层 ';' 或 ':'
            size_t sep = NPOS; bool isRange = false; int dep = 0;
            for (size_t x = k + 1; x + 1 < close; x++) {
                const std::string& t = tokStr(x);
                if (t == "(" || t == "[" || t == "{") dep++;
                else if (t == ")" || t == "]" || t == "}") { if (dep > 0) dep--; }
                else if (dep == 0 && t == ";") { sep = x; break; }
                else if (dep == 0 && t == ":") { sep = x; isRange = true; break; }
            }
            if (sep == NPOS) sep = close - 1;
            ParseForInit(k + 1, sep, isRange);
            i = close; continue;
        }
        if (s == "if" || s == "while" || s == "switch" || s == "do" || s == "else" || s == "try" || s == "catch") {
            size_t k = i + 1;
            if (tokStr(k) == "(") { size_t e = MatchParen(k); if (e != NPOS) { i = e; continue; } }
            i = i + 1; continue;
        }
        // 孤立宏 token (如 _GLIBCXX_BEGIN_NAMESPACE_CXX11): 后跟关键字/标点时跳过,
        // 否则可能是类型 (如模板参数 _T1), 交给声明解析处理
        if (m_toks[i].k == Tok::Ident && IsNoiseName(m_toks[i].s) &&
            !m_aliases.count(m_toks[i].s) && !m_types.count(m_toks[i].s)) {
            size_t j = i + 1;
            bool nextPlainIdent = (j < N && m_toks[j].k == Tok::Ident &&
                                   !IsKeywordName(m_toks[j].s) && !IsBadPrefixIdent(m_toks[j].s));
            if (!nextPlainIdent) { i++; continue; }
        }
        {
            size_t adv = TryDecl(i);
            if (adv > i) { i = adv; continue; }
            size_t e = SkipStatement(i);
            i = (e > i) ? e : i + 1;
        }
    }
    // Qualified class declarations in headers can be only partially understood
    // (e.g. "friend class std::X"). A real namespace must never survive as a
    // spurious class/alias in the lightweight type index.
    m_globals.erase(std::remove_if(m_globals.begin(), m_globals.end(), [&](const CSym& symbol) {
        if (symbol.kind != CSymKind::Type && symbol.kind != CSymKind::Enum) return false;
        return m_namespaces.count(symbol.ns.empty() ? symbol.name : symbol.ns + "::" + symbol.name) != 0;
    }), m_globals.end());
    for (const auto& ns : m_namespaces) if (ns.find("::") == std::string::npos) {
        m_types.erase(ns); m_aliases.erase(ns); m_knownTypes.erase(ns);
    }
}

void CodeModel::Ensure(const std::string& text) {
    size_t h = std::hash<std::string>{}(text);
    if (m_valid && h == m_hash) return;
    Rebuild(text);
}

// ===================================================================
//  查询
// ===================================================================
const CTypeInfo* CodeModel::FindType(const std::string& raw) const {
    if (m_namespaces.count(raw)) return nullptr;
    std::string t = NormType(raw);
    if (m_namespaces.count(t)) return nullptr;
    for (int d = 0; d < 6 && !t.empty(); d++) {
        auto it = m_types.find(t);
        if (it != m_types.end()) return &it->second;
        auto al = m_aliases.find(t);
        if (al == m_aliases.end()) return nullptr;
        std::string nt = NormType(al->second);
        if (nt == t) return nullptr;
        t = nt;
    }
    return nullptr;
}

bool CodeModel::CollectMembers(const std::string& typeName, std::vector<CMember>& out) const {
    const CTypeInfo* ti = FindType(typeName);
    if (!ti) return false;
    std::unordered_set<std::string> seen;
    std::function<void(const CTypeInfo*, int)> rec = [&](const CTypeInfo* t, int depth) {
        if (!t || depth > 5) return;
        for (auto& m : t->members) if (seen.insert(m.name).second) out.push_back(m);
        for (auto& b : t->bases) { const CTypeInfo* bi = FindType(b); if (bi) rec(bi, depth + 1); }
    };
    rec(ti, 0);
    return true;
}

std::string CodeModel::ThisTypeAtImpl(size_t cursorPos) const {
    for (auto& f : m_funcScopes)
        if (cursorPos >= f.start && cursorPos <= f.end && !f.thisType.empty()) return f.thisType;
    for (auto& c : m_classScopes)
        if (cursorPos >= c.start && cursorPos <= c.end) return c.cls;
    return "";
}

std::string CodeModel::ResolveVarType(const std::string& name, size_t cursorPos) const {
    if (name.empty()) return "";
    if (name == "this") return ThisTypeAtImpl(cursorPos);
    if (m_queryDepth > 6) return "";
    const CSym* best = nullptr;
    for (auto& l : m_locals) {
        if (l.name != name) continue;
        if (l.declPos > cursorPos) continue;
        if (l.scopeStart > cursorPos) continue;
        if (l.scopeEnd != 0 && cursorPos > l.scopeEnd) continue;
        if (!best || l.declPos > best->declPos) best = &l;
    }
    if (best) {
        if (best->type.compare(0, 4, "auto") == 0) {
            std::string t = EvalExprType(best->initExpr, best->declPos);
            if (!t.empty()) return t;
        }
        return best->type;
    }
    std::vector<CSym> visible; FilterVisibleSymbols(cursorPos,m_globals,visible);
    for (auto& g : visible) {
        if (g.name != name) continue;
        if (g.kind != CSymKind::Variable && g.kind != CSymKind::Enumerator && g.kind != CSymKind::Parameter) continue;
        if (g.type.compare(0, 4, "auto") == 0) {
            std::string t = EvalExprType(g.initExpr, g.declPos);
            if (!t.empty()) return t;
        }
        return g.type;
    }
    std::string tc = ThisTypeAtImpl(cursorPos);
    if (!tc.empty()) {
        std::vector<CMember> mem;
        if (CollectMembers(tc, mem))
            for (auto& m : mem) if (m.name == name) return m.type;
    }
    return "";
}

std::string CodeModel::EvalExprType(const std::string& expr, size_t cursorPos) const {
    if (m_queryDepth > 6) return "";
    ++m_queryDepth;
    struct Dec { int& d; Dec(int& x) : d(x) {} ~Dec() { --d; } } dec(m_queryDepth);

    std::vector<ETok> ts;
    TokenizeExpr(expr, ts);
    size_t i = 0;
    while (i < ts.size() && (ts[i].s == "*" || ts[i].s == "&")) i++;
    if (i >= ts.size() || ts[i].k != ETok::Id) return "";
    std::string cur;
    bool nsPending = false;
    std::string namespacePrefix;
    auto isNamespace = [&](const std::string& name) {
        if (m_namespaces.count(name)) return true;
        for (const auto& symbol : m_globals)
            if (symbol.ns == name || symbol.ns.rfind(name + "::", 0) == 0) return true;
        return false;
    };
    {
        std::string id = ts[i].s; i++;
        if (id == "this") {
            cur = ThisTypeAtImpl(cursorPos);
        } else if (i < ts.size() && ts[i].s == "::" && (isNamespace(id) || !FindType(id))) {
            cur.clear();
            nsPending = true;
            namespacePrefix = id;
        } else if (i < ts.size() && ts[i].s == "::" && FindType(id)) {
            cur = id;
        } else {
            cur = ResolveVarType(id, cursorPos);
            if (cur.empty()) {   // 函数调用结果: 用其返回类型
                std::vector<CSym> visible; FilterVisibleSymbols(cursorPos,m_globals,visible);
                for (auto& g : visible)
                    if (g.name == id && g.kind == CSymKind::Function && !g.type.empty()) { cur = g.type; break; }
            }
        }
    }
    auto MemberTypeOf = [&](const std::string& type, const std::string& member) -> std::string {
        std::vector<CMember> mem;
        if (CollectMembers(type, mem))
            for (auto& m : mem) if (m.name == member) return m.type;
        return "";
    };
    while (i < ts.size()) {
        const std::string& s = ts[i].s;
        if (s == "::" || s == "." || s == "->") {
            if (i + 1 >= ts.size() || ts[i + 1].k != ETok::Id) break;
            std::string m = ts[i + 1].s; i += 2;
            if (nsPending) {
                // A qualified name can denote an object or function, not just
                // a type: std::cin is an istream, not a class named "cin".
                // Do not use ResolveVarType here: a local of the same name
                // must not shadow an explicitly qualified global symbol.
                const CSym* symbol = nullptr;
                const CSym* systemFallback = nullptr;
                for (const auto& global : m_globals) {
                    if (global.name != m || (global.kind != CSymKind::Variable &&
                        global.kind != CSymKind::Function && global.kind != CSymKind::Type &&
                        global.kind != CSymKind::Enum && global.kind != CSymKind::Enumerator)) continue;
                    if (global.ns == namespacePrefix) { symbol = &global; break; }
                    // libstdc++ namespace macros are not always preserved by
                    // the lightweight parser; allow its header-only fallback.
                    if (m_system && namespacePrefix == "std" &&
                        (global.ns.empty() || global.ns.rfind("std::__", 0) == 0)) systemFallback = &global;
                }
                if (!symbol) symbol = systemFallback;
                if (symbol) {
                    cur = (symbol->kind == CSymKind::Type || symbol->kind == CSymKind::Enum) ? m : symbol->type;
                    if (cur.compare(0, 4, "auto") == 0) cur = EvalExprType(symbol->initExpr, symbol->declPos);
                    nsPending = false;
                } else if (i < ts.size() && ts[i].s == "::") {
                    namespacePrefix += "::" + m;
                } else {
                    return ""; // Let the caller query the included-header model.
                }
            } else if (s == "::") {
                const CTypeInfo* ti = FindType(cur);
                if (!ti) { cur = m; }             // 命名空间链
                else { std::string nt = MemberTypeOf(cur, m); cur = nt.empty() ? m : nt; }
            } else {
                cur = MemberTypeOf(cur, m);
            }
        } else if (s == "(") {
            i++; int d = 1; while (i < ts.size() && d > 0) { if (ts[i].s == "(") d++; else if (ts[i].s == ")") d--; i++; }
        } else if (s == "[") {
            i++; int d = 1; while (i < ts.size() && d > 0) { if (ts[i].s == "[") d++; else if (ts[i].s == "]") d--; i++; }
        } else if (s == "*" || s == "&" || s == ",") {
            i++;
        } else break;
    }
    if (nsPending) return "";
    return NormType(cur);
}

bool CodeModel::FindFunctionSig(const std::string& name, const std::string& objType,
                                std::string& sig, std::string& retType) const {
    if (!objType.empty()) {
        std::vector<CMember> mem;
        if (CollectMembers(objType, mem))
            for (auto& m : mem)
                if (m.name == name && !m.sig.empty()) { sig = m.sig; retType = m.type; return true; }
        return false;
    }
    for (auto& g : m_globals)
        if (g.name == name && g.kind == CSymKind::Function && !g.sig.empty()) { sig = g.sig; retType = g.type; return true; }
    return false;
}

// 跳转定义: 作用域感知地求 name 的声明偏移
bool CodeModel::FindDeclPos(const std::string& name, size_t cursorPos, size_t& outPos) const {
    if (name.empty() || name == "this") return false;
    // 0) 别名 (typedef / using): 解析到真实类型后再定位
    //    如 std::string -> basic_string (跳到 basic_string 的定义, 而非 stringfwd.h 的 typedef)
    auto al = m_aliases.find(name);
    if (al != m_aliases.end() && m_queryDepth < 8) {
        const std::string& tgt = al->second;
        if (!tgt.empty() && tgt != name) {
            m_queryDepth++;
            bool ok = FindDeclPos(tgt, cursorPos, outPos);
            m_queryDepth--;
            if (ok) return true;
        }
    }
    // 1) 可见局部符号: 取在光标前声明且作用域包含光标、声明位置最靠后的那个 (最近优先)
    const CSym* best = nullptr;
    for (auto& l : m_locals) {
        if (l.name != name) continue;
        if (l.declPos > cursorPos) continue;
        if (l.scopeStart > cursorPos) continue;
        if (l.scopeEnd != 0 && cursorPos > l.scopeEnd) continue;
        if (!best || l.declPos > best->declPos) best = &l;
    }
    if (best) { outPos = best->declPos; return true; }
    // 2) 全局符号 (函数/变量/类型/枚举值). 同名多个时优先"有定义体"的, 避免落到前置声明
    best = nullptr;
    for (auto& g : m_globals) {
        if (g.name != name) continue;
        if (!best) { best = &g; continue; }
        if (g.defined && !best->defined) best = &g;
    }
    if (best) { outPos = best->declPos; return true; }
    // 3) 类型别名 (typedef / using): 目标未找到时退回到别名自身的声明处
    auto ap = m_aliasPos.find(name);
    if (ap != m_aliasPos.end()) { outPos = ap->second; return true; }
    // 4) 所在类的成员
    std::string tc = ThisTypeAtImpl(cursorPos);
    if (!tc.empty()) {
        std::vector<CMember> mem;
        if (CollectMembers(tc, mem))
            for (auto& m : mem) if (m.name == name) { outPos = m.declPos; return true; }
    }
    return false;
}

bool CodeModel::FindMemberDeclPos(const std::string& typeName, const std::string& member, size_t& outPos) const {
    std::vector<CMember> mem;
    if (!CollectMembers(typeName, mem)) return false;
    for (auto& m : mem) if (m.name == member) { outPos = m.declPos; return true; }
    return false;
}

void CodeModel::FilterVisibleSymbols(size_t cursorPos, const std::vector<CSym>& candidates,
                                     std::vector<CSym>& out) const {
    out.clear();
    std::string current;
    size_t latest=0;
    for(const auto& scope:m_namespaceScopes)
        if(cursorPos>=scope.start && cursorPos<=scope.end && scope.start>=latest) { current=scope.name; latest=scope.start; }
    std::unordered_set<std::string> visible, names;
    visible.insert("");
    for(std::string owner=current;!owner.empty();) {
        visible.insert(owner); auto split=owner.rfind("::");
        owner=split==std::string::npos ? "" : owner.substr(0,split);
    }
    // Resolve namespace-scoped and block-scoped imports, including transitive
    // using directives. A directive in a sibling/finished block cannot leak.
    bool changed=true;
    for(size_t pass=0;changed && pass<=m_usingImports.size()+m_inlineNamespaces.size();++pass) {
        changed=false;
        for(const auto& ns:m_inlineNamespaces) {
            auto split=ns.rfind("::"); std::string parent=split==std::string::npos ? "" : ns.substr(0,split);
            if(visible.count(parent)) changed=visible.insert(ns).second || changed;
        }
        for(const auto& import:m_usingImports) {
            if(import.declPos>cursorPos || !visible.count(import.owner)) continue;
            if(import.local && (cursorPos<import.start || cursorPos>import.end)) continue;
            std::string target=import.target;
            if(!import.absolute && !import.owner.empty()) {
                auto sep=target.rfind("::");
                std::string targetNs=import.namespc ? target : sep==std::string::npos ? "" : target.substr(0,sep);
                if(m_namespaces.count(import.owner+"::"+targetNs)) target=import.owner+"::"+target;
            }
            if(import.namespc) changed=visible.insert(target).second || changed;
            else names.insert(target);
        }
    }
    for(const auto& symbol:candidates)
        if(visible.count(symbol.ns) || names.count(symbol.ns.empty() ? symbol.name : symbol.ns+"::"+symbol.name)) out.push_back(symbol);
}

void CodeModel::VisibleSymbols(size_t cursorPos, std::vector<CSym>& out) const {
    out.clear();
    FilterVisibleSymbols(cursorPos,m_globals,out);
    for (auto& l : m_locals) {
        if (l.declPos > cursorPos) continue;
        if (l.scopeStart > cursorPos) continue;
        if (l.scopeEnd != 0 && cursorPos > l.scopeEnd) continue;
        out.push_back(l);
    }
    std::string tc = ThisTypeAtImpl(cursorPos);
    if (!tc.empty()) {
        std::vector<CMember> mem;
        CollectMembers(tc, mem);
        std::unordered_set<std::string> have;
        for (auto& s : out) have.insert(s.name);
        for (auto& m : mem) {
            if (!have.insert(m.name).second) continue;
            CSym s; s.name = m.name; s.type = m.type; s.sig = m.sig;
            s.kind = (m.kind == CSymKind::Function) ? CSymKind::Function : CSymKind::Member;
            out.push_back(std::move(s));
        }
    }
}

std::vector<CSym> CodeModel::AllSymbols() const {
    std::vector<CSym> out = m_globals;
    std::unordered_set<std::string> have;
    for (auto& s : out) have.insert(s.name);
    for (auto& kv : m_types) {
        const CTypeInfo& t = kv.second;
        if (t.name.empty() || t.name[0] == '$') continue;
        if (m_system && IsNoiseName(t.name)) continue;
        if (!have.insert(t.name).second) continue;
        CSym s; s.name = t.name; s.type = t.name;
        s.kind = t.isEnum ? CSymKind::Enum : CSymKind::Type;
        out.push_back(std::move(s));
    }
    for (auto& kv : m_aliases) {
        if (m_system && IsNoiseName(kv.first)) continue;
        if (!have.insert(kv.first).second) continue;
        CSym s; s.name = kv.first; s.type = kv.second; s.kind = CSymKind::Type;
        out.push_back(std::move(s));
    }
    return out;
}

void CodeModel::NamespaceSymbols(const std::string& ns, bool includeSub, std::vector<CSym>& out) const {
    out.clear();
    if (ns.empty()) return;
    for (auto& g : m_globals) {
        if (g.ns == ns) { out.push_back(g); continue; }
        if (includeSub && g.ns.size() > ns.size() &&
            g.ns.compare(0, ns.size(), ns) == 0 && g.ns[ns.size()] == ':')
            out.push_back(g);
    }
}

void CodeModel::DeclarationSites(const CSym& symbol, std::vector<CSym>& out) const {
    out.clear(); std::unordered_set<size_t> positions;
    auto add=[&](const CSym& candidate) {
        if(candidate.name==symbol.name && candidate.ns==symbol.ns && candidate.kind==symbol.kind && positions.insert(candidate.declPos).second) out.push_back(candidate);
    };
    add(symbol);
    for(const auto& candidate:m_declarationSites) add(candidate);
    for(const auto& candidate:m_globals) add(candidate);
}
