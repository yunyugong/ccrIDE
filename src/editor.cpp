#include "language_host.h"
#include "line_changes.h"
#include "editor.h"
#include "app.h"
#include <shellapi.h>
#include "utils.h"
#include "ui_helpers.h"
#include "symbol_index.h"
#include <imgui_internal.h>   // ImFont 完整定义 / ImRect
#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <unordered_set>
#include <deque>
#include <filesystem>
#include <cwctype>
#include <nlohmann/json.hpp>

// ===================================================================
//  静态数据: 关键字 / 常用类型
// ===================================================================
static const std::unordered_set<std::string>& KeywordSet() {
    static const std::unordered_set<std::string> s = {
        "alignas","alignof","and","and_eq","asm","auto","bitand","bitor","bool","break",
        "case","catch","char","char8_t","char16_t","char32_t","class","co_await","co_return",
        "co_yield","compl","concept","const","consteval","constexpr","constinit","const_cast",
        "continue","decltype","default","delete","do","double","dynamic_cast","else","enum",
        "explicit","export","extern","false","float","for","friend","goto","if","inline",
        "int","long","mutable","namespace","new","noexcept","not","not_eq","nullptr",
        "operator","or","or_eq","private","protected","public","register","reinterpret_cast",
        "requires","return","short","signed","sizeof","static","static_assert","static_cast",
        "struct","switch","template","this","thread_local","throw","true","try","typedef",
        "typeid","typename","union","unsigned","using","virtual","void","volatile","wchar_t",
        "while","xor","xor_eq"
    };
    return s;
}

static const std::unordered_set<std::string>& TypeNameSet() {
    static const std::unordered_set<std::string> s = {
        "int8_t","int16_t","int32_t","int64_t","uint8_t","uint16_t","uint32_t","uint64_t",
        "size_t","ssize_t","ptrdiff_t","intptr_t","uintptr_t","intmax_t","uintmax_t",
        "string","wstring","vector","map","unordered_map","set","unordered_set","list",
        "deque","queue","stack","pair","tuple","shared_ptr","unique_ptr","weak_ptr",
        "function","optional","variant","any","span","array","string_view","fstream",
        "ifstream","ofstream","stringstream","iostream","ostream","istream","FILE","va_list",
        "chrono","thread","mutex","atomic","lock_guard","unique_lock","condition_variable",
        "error_code","filesystem","path"
    };
    return s;
}

// 语言内置类型 (无需包含任何头文件即可使用) —— 库类型一律靠解析头文件获得
static const std::unordered_set<std::string>& BuiltinTypeSet() {
    static const std::unordered_set<std::string> s = {
        "void","bool","char","char8_t","char16_t","char32_t","wchar_t","short","int",
        "long","float","double","signed","unsigned","size_t","ssize_t","ptrdiff_t",
        "intptr_t","uintptr_t","intmax_t","uintmax_t","int8_t","int16_t","int32_t",
        "int64_t","uint8_t","uint16_t","uint32_t","uint64_t"
    };
    return s;
}

// 是否已配置编译器 (能拿到 include 路径 -> 可解析头文件; 否则回退内置表)
static bool CompilerAvail() { return SymbolIndex::I().HasIncludeDirs(); }

// ===================================================================
//  基础
// ===================================================================
void CodeEditor::SetText(const std::string& t) {
    m_carets.clear();
    m_text = t;
    m_savedBaseline = t;
    m_cursor = 0;
    m_selAnchor = 0;
    m_selActive = false;
    m_undo.clear();
    m_redo.clear();
    m_scrollX = m_scrollY = 0;
    InvalidateAll();
}

void CodeEditor::SetPath(const std::wstring& p) {
    m_path = p;
    if (!p.empty()) m_name = utils::WToU8(utils::FileBase(p));
}

bool CodeEditor::SaveToPath(const std::wstring& p) {
    // GBK 文件保存时保持原编码, 其余写 UTF-8
    std::string out = (m_enc == 1) ? utils::Utf8ToGbk(m_text) : m_text;
    if (utils::WriteFileU8(p, out)) {
        m_path = p;
        m_name = utils::WToU8(utils::FileBase(p));
        m_dirty = false;
        m_savedBaseline = m_text;
        m_lineChangesDirty = true;
        return true;
    }
    return false;
}

bool CodeEditor::SaveAs() {
    std::wstring path;
    if (!utils::SaveFileDialog(g_app.dataDir, L"未命名.cpp", path)) return false;
    return SaveToPath(path);
}

bool CodeEditor::Save() {
    if (!HasPath()) return SaveAs();
    return SaveToPath(m_path);
}

std::string CodeEditor::GetSelectionText() const {
    if (!m_selActive) return std::string();
    return m_text.substr(SelBegin(), SelEnd() - SelBegin());
}

// ===================================================================
//  度量
// ===================================================================
static int EditorTabWidth(){return std::clamp(g_app.settings.tabWidth,2,8);}
int CodeEditor::TabWidth() const{return EditorTabWidth();}
std::string CodeEditor::IndentUnit() const{
    return g_app.settings.insertSpaces?std::string(TabWidth(),' '):"\t";
}
bool CodeEditor::ShouldAutoComplete() const{
    if(!g_app.settings.codeCompletion||!g_app.settings.autoCompletion)return false;
    bool include=false,quote=false;size_t start=CompPrefixStart(include,quote);
    char previous=start?m_text[start-1]:0,prior=start>1?m_text[start-2]:0;
    bool member=previous=='.'||(previous=='>'&&prior=='-')||(previous==':'&&prior==':');
    return include||member||m_cursor-start>=size_t(std::clamp(g_app.settings.completionMinChars,1,4));
}
float CodeEditor::LineH() const {
    ImFont* f = g_app.editorFont ? g_app.editorFont : ImGui::GetFont();
    float fs = f ? f->LegacySize : 16.0f;
    return std::floor(fs * std::clamp(g_app.settings.lineHeightPercent,110,180)/100.0f) + 2.0f;
}

float CodeEditor::CharW() const {
    ImFont* f = g_app.editorFont ? g_app.editorFont : ImGui::GetFont();
    float fs = f ? f->LegacySize : 16.0f;
    return f->CalcTextSizeA(fs, FLT_MAX, -1.0f, "M", nullptr, nullptr).x;
}

float CodeEditor::GutterW() const {
    size_t n = m_lines.size();
    int digits = 1;
    while (n >= 10) { n /= 10; digits++; }
    if(!g_app.settings.showLineNumbers)return 30.0f;
    float w = digits * CharW() + 30.0f;
    return w < 48.0f ? 48.0f : w;
}

// ===================================================================
//  位置计算
// ===================================================================
size_t CodeEditor::PrevChar(size_t pos) const {
    if (pos == 0) return 0;
    size_t p = pos - 1;
    while (p > 0 && ((unsigned char)m_text[p] & 0xC0) == 0x80) p--;
    return p;
}

size_t CodeEditor::NextChar(size_t pos) const {
    if (pos >= m_text.size()) return m_text.size();
    size_t p = pos + 1;
    while (p < m_text.size() && ((unsigned char)m_text[p] & 0xC0) == 0x80) p++;
    return p;
}

bool CodeEditor::IsIdentChar(char c) const {
    return std::isalnum((unsigned char)c) || c == '_' || (unsigned char)c >= 0x80;
}
bool CodeEditor::IsIdentStart(char c) const {
    return std::isalpha((unsigned char)c) || c == '_' || (unsigned char)c >= 0x80;
}

size_t CodeEditor::WordLeft(size_t pos) const {
    while (pos > 0 && !IsIdentChar(m_text[PrevChar(pos)])) pos = PrevChar(pos);
    while (pos > 0 && IsIdentChar(m_text[PrevChar(pos)])) pos = PrevChar(pos);
    return pos;
}

size_t CodeEditor::WordRight(size_t pos) const {
    size_t n = m_text.size();
    while (pos < n && IsIdentChar(m_text[pos])) pos = NextChar(pos);
    while (pos < n && !IsIdentChar(m_text[pos])) pos = NextChar(pos);
    return pos;
}

size_t CodeEditor::LineOf(size_t pos) const {
    auto it = std::upper_bound(m_lines.begin(), m_lines.end(), pos);
    return (size_t)(it - m_lines.begin()) - 1;
}

size_t CodeEditor::LineStart(size_t line) const { return m_lines[line]; }

size_t CodeEditor::LineEnd(size_t line) const {
    if (line + 1 < m_lines.size()) {
        size_t e = m_lines[line + 1];
        if (e > 0 && m_text[e - 1] == '\n') e--;
        return e;
    }
    return m_text.size();
}

size_t CodeEditor::VisualCol(size_t pos) const {
    size_t line = LineOf(pos), ls = LineStart(line);
    size_t col = 0;
    for (size_t i = ls; i < pos && i < m_text.size(); i = NextChar(i)) col=m_text[i]=='\t'?(col/TabWidth()+1)*TabWidth():col+1;
    return col;
}

size_t CodeEditor::ColToByte(size_t line, size_t col) const {
    size_t ls = LineStart(line), le = LineEnd(line);
    size_t i = ls, c = 0;
    while (i < le && c < col) {
        size_t next=m_text[i]=='\t'?(c/TabWidth()+1)*TabWidth():c+1;
        if(next>col&&col-c<(next-c+1)/2)break;
        i=NextChar(i);c=next;
    }
    return i;
}

size_t CodeEditor::PosAtLineCol(size_t line, size_t col) const {
    if (line >= m_lines.size()) return m_text.size();
    return ColToByte(line, col);
}

void CodeEditor::ClampCursor() {
    if (m_cursor > m_text.size()) m_cursor = m_text.size();
    if (m_selAnchor > m_text.size()) m_selAnchor = m_text.size();
}

// ===================================================================
//  重建索引
// ===================================================================
void CodeEditor::RebuildLines() {
    m_lines.clear();
    m_lines.push_back(0);
    for (size_t i = 0; i < m_text.size(); i++)
        if (m_text[i] == '\n') m_lines.push_back(i + 1);
    m_linesDirty = false;
    m_visDirty = true;   // 行数变化 -> 可视行缓存失效 (否则撤销后残留旧行号, 绘制越界/少行)
}

void CodeEditor::RebuildCommentCache() {
    m_lineInComment.assign(m_lines.size(), false);
    bool in = false;
    const char* b = m_text.data();
    for (size_t li = 0; li < m_lines.size(); li++) {
        m_lineInComment[li] = in;
        size_t ls = LineStart(li), le = LineEnd(li);
        size_t i = ls;
        while (i < le) {
            if (in) {
                if (b[i] == '*' && i + 1 < le && b[i + 1] == '/') { in = false; i += 2; }
                else i++;
            } else {
                if (b[i] == '/' && i + 1 < le && b[i + 1] == '*') { in = true; i += 2; }
                else i++;
            }
        }
    }
    m_commentDirty = false;
}

// 构建某行的分词缓存 (TokenizeLine 依赖该行起始是否处于块注释中)
void CodeEditor::EnsureTokens(size_t line) const {
    if ((int)m_tokCache.size() < (int)m_lines.size()) {
        m_tokCache.resize(m_lines.size());
        m_tokValid.assign(m_lines.size(), false);
    }
    if (line >= m_tokValid.size() || m_tokValid[line]) return;
    bool inBlock = (line < m_lineInComment.size()) && m_lineInComment[line];
    m_tokCache[line] = TokenizeLine(line, inBlock);
    m_tokValid[line] = true;
}

// 偏移是否落在字符串/注释里 (用分词结果判定, 供括号匹配高亮排除字面量里的括号)
bool CodeEditor::IsPosInStringOrComment(size_t pos) const {
    if (m_lines.empty() || pos >= m_text.size()) return false;
    size_t line = LineOf(pos);
    if (line >= m_lines.size()) return false;
    EnsureTokens(line);
    size_t ls = LineStart(line);
    for (auto& t : m_tokCache[line]) {
        if (t.type != TokenKind::String && t.type != TokenKind::Comment) continue;
        if (pos >= ls + t.start && pos < ls + t.start + t.len) return true;
    }
    return false;
}

void CodeEditor::InvalidateAll() {
    m_lineChangesDirty = true;
    m_decorationDirty = true;
    m_linesDirty = true;
    m_commentDirty = true;
    m_identsDirty = true;
    m_maxLineDirty = true;
    std::fill(m_tokValid.begin(), m_tokValid.end(), false);   // 分词缓存失效
    if (m_findOpen) RecomputeMatches();
}

// ===================================================================
//  编辑操作
// ===================================================================
void CodeEditor::PushUndo(size_t pos, const std::string& removed, const std::string& inserted) {
    if (!m_undo.empty()) {
        UndoOp& last = m_undo.back();
        if (removed.empty() && last.removed.empty() && last.pos + last.inserted.size() == pos) {
            last.inserted += inserted;   // 合并连续输入
            m_redo.clear();
            return;
        }
        if (inserted.empty() && last.inserted.empty() && last.pos == pos + removed.size()) {
            last.pos = pos;              // 合并连续删除
            last.removed = removed + last.removed;
            m_redo.clear();
            return;
        }
    }
    m_undo.push_back({ pos, removed, inserted });
    m_redo.clear();
    if ((int)m_undo.size() > MAX_UNDO) m_undo.erase(m_undo.begin());
}

void CodeEditor::RefreshLineChanges() {
    if (!m_lineChangesDirty) return;
    m_originalLines = BaselineLines(m_savedBaseline, m_text);
    m_lineChangesDirty = false;
}
void CodeEditor::ToggleBreakpoint(int line) {
    auto debug=g_app.debugger.Snapshot();
    if(debug.active && !debug.stopped) { g_app.console.Append("[调试] 请先暂停再修改断点\n",IM_COL32(255,190,60,255)); return; }
    auto it=std::lower_bound(m_breakpoints.begin(),m_breakpoints.end(),line);
    if(it!=m_breakpoints.end() && *it==line) m_breakpoints.erase(it); else m_breakpoints.insert(it,line);
    if(debug.active) g_app.debugger.SetBreakpoints(g_app.DebugBreakpoints());
}

void CodeEditor::InsertAt(size_t pos, const std::string& s, bool record) {
    if (s.empty()) return;
    InvalidateFolds();   // 编辑即全部展开 (简单安全)
    if (record) PushUndo(pos, "", s);
    int row = (int)LineOf(pos);
    int insertedLines=(int)std::count(s.begin(),s.end(),'\n');
    for(auto& point:m_breakpoints) if(insertedLines && LineStart((size_t)(point-1))>=pos) point+=insertedLines;
    m_text.insert(pos, s);
    // 书签行号跟随: 插入 k 个换行 -> 之后的书签下移 k
    int k = (int)std::count(s.begin(), s.end(), '\n');
    if (k > 0)
        for (auto& bm : m_bookmarks)
            if (bm >= row) bm += k;
    InvalidateAll();     // 先置脏再立即重建, Draw 不再重复重建
    RebuildLines();
    m_cursorMoved = true;
    g_app.MarkDiag();
}

void CodeEditor::EraseRange(size_t a, size_t b, bool record) {
    if (b <= a) return;
    InvalidateFolds();   // 编辑即全部展开 (简单安全)
    if (record) PushUndo(a, m_text.substr(a, b - a), "");
    int ra = (int)LineOf(a), rb = (int)LineOf(b);
    if(rb>ra) {
        m_breakpoints.erase(std::remove_if(m_breakpoints.begin(),m_breakpoints.end(),[&](int line){ size_t start=LineStart((size_t)(line-1)); return start>=a&&start<b; }),m_breakpoints.end());
        for(auto& line:m_breakpoints) if(line>rb) line-=rb-ra;
        m_breakpoints.erase(std::unique(m_breakpoints.begin(),m_breakpoints.end()),m_breakpoints.end());
    }
    m_text.erase(a, b - a);
    // 书签行号跟随: 删除区间的书签移除, 之后的上移
    int k = rb - ra;
    if (k > 0) {
        m_bookmarks.erase(
            std::remove_if(m_bookmarks.begin(), m_bookmarks.end(),
                           [&](int v) { return v > ra && v <= rb; }),
            m_bookmarks.end());
        for (auto& bm : m_bookmarks)
            if (bm > rb) bm -= k;
    }
    InvalidateAll();
    RebuildLines();
    m_cursorMoved = true;
    g_app.MarkDiag();
}

void CodeEditor::DeleteSelection() {
    if (m_selActive && SelBegin() != SelEnd()) {
        size_t a = SelBegin();
        EraseRange(a, SelEnd());
        m_cursor = a;
        m_selAnchor = a;
        m_selActive = false;
    }
}

void CodeEditor::InsertText(const std::string& s) {
    if(!m_carets.empty()){MultiReplace(s);return;}
    if (s.empty()) return;
    if (m_selActive && SelBegin() != SelEnd()) {
        // 选区被输入替换: 删除+插入合并为单步撤销 (否则要按两次 Ctrl+Z)
        size_t a = SelBegin(), b = SelEnd();
        PushUndo(a, m_text.substr(a, b - a), s);
        EraseRange(a, b, false);
        InsertAt(a, s, false);
        m_cursor = a + s.size();
    } else {
        InsertAt(m_cursor, s);
        m_cursor += s.size();
    }
    m_selAnchor = m_cursor;
    m_selActive = false;
    ClampCursor();
    m_cursorMoved = true;
}

void CodeEditor::DoBackspace() {
    if (m_selActive && SelBegin() != SelEnd()) { DeleteSelection(); return; }
    if (m_cursor == 0) return;

    // 空括号对一起删
    if (g_app.settings.autoCloseBrackets && m_cursor < m_text.size()) {
        size_t p = PrevChar(m_cursor);
        char before = m_text[p], after = m_text[m_cursor];
        if ((before == '(' && after == ')') || (before == '[' && after == ']') ||
            (before == '{' && after == '}') || (before == '"' && after == '"') ||
            (before == '\'' && after == '\'')) {
            EraseRange(p, NextChar(m_cursor));
            m_cursor = p;
            m_selAnchor = m_cursor;
            m_cursorMoved = true;
            return;
        }
    }

    // 光标前只有空格 -> 按 4 格删除
    size_t line = LineOf(m_cursor), ls = LineStart(line);
    std::string before = m_text.substr(ls, m_cursor - ls);
    if (!before.empty() && before.find_first_not_of(" \t") == std::string::npos) {
        size_t n = before.size();
        size_t trailing=0;while(trailing<n&&before[n-trailing-1]==' ')++trailing;
        size_t step=VisualCol(m_cursor)%TabWidth();if(!step)step=TabWidth();
        size_t del=before.back()=='\t'?1:std::min(trailing,step);
        EraseRange(m_cursor - del, m_cursor);
        m_cursor -= del;
        m_selAnchor = m_cursor;
        m_cursorMoved = true;
        return;
    }

    size_t p = PrevChar(m_cursor);
    EraseRange(p, m_cursor);
    m_cursor = p;
    m_selAnchor = m_cursor;
    m_cursorMoved = true;
}

void CodeEditor::DoDelete() {
    if (m_selActive && SelBegin() != SelEnd()) { DeleteSelection(); return; }
    if (m_cursor >= m_text.size()) return;
    size_t n = NextChar(m_cursor);
    EraseRange(m_cursor, n);
    m_cursorMoved = true;
}

void CodeEditor::DoEnter() {
    DeleteSelection();
    if(!g_app.settings.autoIndent){InsertText("\n");return;}
    size_t start=LineStart(LineOf(m_cursor));
    std::string before=m_text.substr(start,m_cursor-start);
    size_t count=before.find_first_not_of(" \t");if(count==std::string::npos)count=before.size();
    std::string indent=before.substr(0,count),code=before.substr(count);
    if(g_app.settings.insertSpaces){
        size_t columns=0;for(char c:indent)columns=c=='\t'?(columns/TabWidth()+1)*TabWidth():columns+1;
        indent.assign(columns,' ');
    }
    std::string next=indent;
    if(!code.empty()&&code.back()=='{')next+=IndentUnit();
    if(!code.empty()&&code.front()=='}'&&!next.empty()){
        size_t remove=next.back()=='\t'?1:std::min(next.size(),size_t(TabWidth()));
        next.resize(next.size()-remove);
    }
    if(!code.empty()&&code.back()=='{'&&m_cursor<m_text.size()&&m_text[m_cursor]=='}'){
        InsertText("\n"+next+"\n"+indent);
        m_cursor-=1+indent.size();m_selAnchor=m_cursor;m_cursorMoved=true;
    }else InsertText("\n"+next);
}
void CodeEditor::DoTab(){
    InsertText(g_app.settings.insertSpaces?std::string(TabWidth()-VisualCol(SelBegin())%TabWidth(),' '):"\t");
}

void CodeEditor::DoAutoBracket(unsigned int c) {
    char ch = (char)c;
    if(!g_app.settings.autoCloseBrackets){InsertText(std::string(1,ch));return;}
    // 闭合括号 ) ] }: 光标后就是同字符 -> 跳过 (typed-over); 否则直接插入
    // (注意: 不能落入 default, 否则自动配对的闭合括号无法跳过, 会插入两个)
    if (ch == ')' || ch == ']' || ch == '}') {
        if (m_cursor < m_text.size() && m_text[m_cursor] == ch) {
            m_cursor = NextChar(m_cursor);
            m_selAnchor = m_cursor;
            m_cursorMoved = true;
        } else {
            InsertText(std::string(1, ch));
        }
        return;
    }
    char close = 0;
    switch (ch) {
        case '(': close = ')'; break;
        case '[': close = ']'; break;
        case '{': close = '}'; break;
        case '"': close = '"'; break;
        case '\'': close = '\''; break;
        default: InsertText(std::string(1, ch)); return;
    }
    // 引号: 光标后就是引号 -> 跳过
    if (ch == close && m_cursor < m_text.size() && m_text[m_cursor] == close) {
        m_cursor = NextChar(m_cursor);
        m_selAnchor = m_cursor;
        m_cursorMoved = true;
        return;
    }
    std::string s(1, ch);
    s += close;
    InsertText(s);
    m_cursor -= 1;   // 光标放中间
    m_selAnchor = m_cursor;
    m_cursorMoved = true;
}

void CodeEditor::Undo() {
    m_carets.clear();
    if (m_undo.empty()) return;
    UndoOp op = m_undo.back();
    m_undo.pop_back();
    // 反向栈保存同一操作: UndoOp 语义 = "把 [pos, pos+inserted) 换成 removed",
    // 因此撤销/重做只是同一操作的两个方向, 必须原样搬移, 不能改写 removed 字段
    m_redo.push_back(op);
    // 替换 [op.pos, op.pos+op.inserted.size()) -> op.removed
    size_t a = op.pos, b = op.pos + op.inserted.size();
    if (b > m_text.size()) b = m_text.size();
    if (b > a) m_text.erase(a, b - a);
    m_text.insert(a, op.removed);
    m_cursor = a + op.removed.size();
    m_selAnchor = m_cursor;
    m_selActive = false;
    InvalidateAll();
    RebuildLines();
    m_cursorMoved = true;
    g_app.MarkDiag();
}

void CodeEditor::Redo() {
    m_carets.clear();
    if (m_redo.empty()) return;
    InvalidateFolds();   // 与编辑一致: 折叠锚点/范围已不可靠, 全部展开
    UndoOp op = m_redo.back();
    m_redo.pop_back();
    m_undo.push_back(op);
    // 替换 [op.pos, op.pos+op.removed.size()) -> op.inserted
    size_t a = op.pos, b = op.pos + op.removed.size();
    if (b > m_text.size()) b = m_text.size();
    if (b > a) m_text.erase(a, b - a);
    m_text.insert(a, op.inserted);
    m_cursor = a + op.inserted.size();
    m_selAnchor = m_cursor;
    m_selActive = false;
    InvalidateAll();
    RebuildLines();
    m_cursorMoved = true;
    g_app.MarkDiag();
}

// ===================================================================
//  光标移动
// ===================================================================
void CodeEditor::MoveLeft(bool sel, bool word) {
    if (sel && !m_selActive) { m_selAnchor = m_cursor; m_selActive = true; }
    if (!sel && m_selActive) { m_cursor = SelBegin(); m_selAnchor = m_cursor; m_selActive = false; m_cursorMoved = true; return; }
    m_cursor = word ? WordLeft(m_cursor) : PrevChar(m_cursor);
    if (!sel) m_selAnchor = m_cursor;
    m_cursorMoved = true;
}

void CodeEditor::MoveRight(bool sel, bool word) {
    if (sel && !m_selActive) { m_selAnchor = m_cursor; m_selActive = true; }
    if (!sel && m_selActive) { m_cursor = SelEnd(); m_selAnchor = m_cursor; m_selActive = false; m_cursorMoved = true; return; }
    m_cursor = word ? WordRight(m_cursor) : NextChar(m_cursor);
    if (!sel) m_selAnchor = m_cursor;
    m_cursorMoved = true;
}

void CodeEditor::MoveUp(bool sel) {
    if (sel && !m_selActive) { m_selAnchor = m_cursor; m_selActive = true; }
    if (!sel && m_selActive) { m_cursor = SelBegin(); m_selAnchor = m_cursor; m_selActive = false; m_cursorMoved = true; return; }
    size_t line = LineOf(m_cursor);
    size_t col = VisualCol(m_cursor);
    if (line > 0) m_cursor = PosAtLineCol(line - 1, col);
    if (!sel) m_selAnchor = m_cursor;
    m_cursorMoved = true;
}

void CodeEditor::MoveDown(bool sel) {
    if (sel && !m_selActive) { m_selAnchor = m_cursor; m_selActive = true; }
    if (!sel && m_selActive) { m_cursor = SelEnd(); m_selAnchor = m_cursor; m_selActive = false; m_cursorMoved = true; return; }
    size_t line = LineOf(m_cursor);
    size_t col = VisualCol(m_cursor);
    if (line + 1 < m_lines.size()) m_cursor = PosAtLineCol(line + 1, col);
    if (!sel) m_selAnchor = m_cursor;
    m_cursorMoved = true;
}

void CodeEditor::MoveHome(bool sel, bool ctrl) {
    if (sel && !m_selActive) { m_selAnchor = m_cursor; m_selActive = true; }
    if (ctrl) {
        m_cursor = 0;
    } else {
        // 智能 Home: 行首 <-> 首个非空白
        size_t ls = LineStart(LineOf(m_cursor)), e = ls;
        while (e < m_text.size() && (m_text[e] == ' ' || m_text[e] == '\t')) e++;
        m_cursor = (m_cursor != e) ? e : ls;
    }
    if (!sel) m_selAnchor = m_cursor;
    m_cursorMoved = true;
}

void CodeEditor::MoveEnd(bool sel, bool ctrl) {
    if (sel && !m_selActive) { m_selAnchor = m_cursor; m_selActive = true; }
    m_cursor = ctrl ? m_text.size() : LineEnd(LineOf(m_cursor));
    if (!sel) m_selAnchor = m_cursor;
    m_cursorMoved = true;
}

void CodeEditor::PageUp(bool sel) {
    float pageLines = (m_drawSize.y > 0 ? m_drawSize.y : 400.0f) / LineH();
    size_t line = LineOf(m_cursor);
    size_t col = VisualCol(m_cursor);
    int row = VisibleRowOfLine((int)line); if (row < 0) row = (int)line;
    int page = (int)pageLines; if (page < 1) page = 1;
    long targetRow = (long)row - page;
    if (targetRow < 0) targetRow = 0;
    m_scrollY = (float)targetRow * LineH();
    m_cursor = PosAtLineCol((size_t)LineOfVisualRow((int)targetRow), col);
    if (!sel) m_selAnchor = m_cursor;
    m_cursorMoved = true;
}

void CodeEditor::PageDown(bool sel) {
    float pageLines = (m_drawSize.y > 0 ? m_drawSize.y : 400.0f) / LineH();
    size_t line = LineOf(m_cursor);
    size_t col = VisualCol(m_cursor);
    int row = VisibleRowOfLine((int)line); if (row < 0) row = (int)line;
    int page = (int)pageLines; if (page < 1) page = 1;
    long targetRow = (long)row + page;
    m_scrollY = (float)targetRow * LineH();
    m_cursor = PosAtLineCol((size_t)LineOfVisualRow((int)targetRow), col);
    if (!sel) m_selAnchor = m_cursor;
    m_cursorMoved = true;
}

void CodeEditor::SelectAll() {
    m_carets.clear();
    m_selAnchor = 0;
    m_cursor = m_text.size();
    m_selActive = true;
    m_cursorMoved = true;
}

void CodeEditor::ToggleBookmark() {
    int line = (int)LineOf(m_cursor);
    auto it = std::lower_bound(m_bookmarks.begin(), m_bookmarks.end(), line);
    if (it != m_bookmarks.end() && *it == line) m_bookmarks.erase(it);
    else m_bookmarks.insert(it, line);
}

void CodeEditor::NextBookmark() {
    if (m_bookmarks.empty()) return;
    int cur = (int)LineOf(m_cursor);
    for (int l : m_bookmarks)
        if (l > cur) { GotoLine(l + 1); return; }
    GotoLine(m_bookmarks.front() + 1);   // 循环到第一个
}

void CodeEditor::PrevBookmark() {
    if (m_bookmarks.empty()) return;
    int cur = (int)LineOf(m_cursor);
    for (auto it = m_bookmarks.rbegin(); it != m_bookmarks.rend(); ++it)
        if (*it < cur) { GotoLine(*it + 1); return; }
    GotoLine(m_bookmarks.back() + 1);    // 循环到最后一个
}

void CodeEditor::ClearAllBookmarks() {
    m_bookmarks.clear();
}

bool CodeEditor::HasBookmark(int line) const {
    return std::binary_search(m_bookmarks.begin(), m_bookmarks.end(), line);
}

// 括号匹配: pos 处为 ()[]{} 之一, 返回匹配括号偏移 / -1
int CodeEditor::FindMatchingBracket(size_t pos) const {
    if (pos >= m_text.size()) return -1;
    char c = m_text[pos];
    char open = 0, close = 0;
    int  dir  = 0;
    switch (c) {
        case '(': open = '('; close = ')'; dir =  1; break;
        case '[': open = '['; close = ']'; dir =  1; break;
        case '{': open = '{'; close = '}'; dir =  1; break;
        case ')': open = '('; close = ')'; dir = -1; break;
        case ']': open = '['; close = ']'; dir = -1; break;
        case '}': open = '{'; close = '}'; dir = -1; break;
        default:  return -1;
    }
    // 原实现把 open/close 都解成同一个字符, 恒返回 -1 (括号高亮从未生效)
    // 正向: 遇 open 加深, 遇 close 回退到 0 即命中; 反向同理镜像 (遇 close 加深, open 回退)
    // 字符串/注释里的括号不参与匹配 (如 "()" 或 // { )
    if (IsPosInStringOrComment(pos)) return -1;   // 起始括号本身在字面量内 -> 无匹配
    int depth = 0;
    for (long long p = (long long)pos; p >= 0 && p < (long long)m_text.size(); p += dir) {
        if (IsPosInStringOrComment((size_t)p)) continue;
        char d = m_text[p];
        if (dir > 0) {
            if (d == open) depth++;
            else if (d == close && --depth == 0) return (int)p;
        } else {
            if (d == close) depth++;
            else if (d == open && --depth == 0) return (int)p;
        }
    }
    return -1;
}

void CodeEditor::GotoLine(int line) {
    if (line < 1) line = 1;
    if ((size_t)line > m_lines.size()) line = (int)m_lines.size();
    m_cursor = LineStart((size_t)line - 1);
    m_selAnchor = m_cursor;
    m_selActive = false;
    // 目标行放到视口上 1/3 处 (折叠时按可视行)
    float vh = (m_drawSize.y > 0) ? m_drawSize.y : 400.0f;
    int r = VisibleRowOfLine((int)line - 1);
    if (r < 0) r = (int)line - 1;
    m_scrollY = std::max(0.0f, r * LineH() - vh * 0.3f);
    m_cursorMoved = false;   // 已手动定位滚动
    CloseCompletion();
    CloseCalltip();
}

// ===================================================================
//  字符 X 坐标 (含 Tab 展开)
// ===================================================================
float CodeEditor::CharX(size_t pos) const {
    ImFont* f = g_app.editorFont ? g_app.editorFont : ImGui::GetFont();
    float fs = f ? f->LegacySize : 16.0f;
    float charW = f->CalcTextSizeA(fs, FLT_MAX, -1.0f, "M", nullptr, nullptr).x;
    size_t line = LineOf(pos), ls = LineStart(line), le = LineEnd(line);
    float x = 0;
    size_t i = ls;
    const char* b = m_text.data();
    while (i < pos && i < le) {
        char c = b[i];
        if (c == '\t') {
            size_t col = (size_t)(x / charW);
            x = (float)((col / EditorTabWidth() + 1) * EditorTabWidth()) * charW;
            i++;
        } else {
            size_t e = NextChar(i);
            x += f->CalcTextSizeA(fs, FLT_MAX, -1.0f, b + i, b + e, nullptr).x;
            i = e;
        }
    }
    return x;
}

// ===================================================================
//  命中测试
// ===================================================================
size_t CodeEditor::HitTest(float contentX, float contentY) const {
    ImFont* f = g_app.editorFont ? g_app.editorFont : ImGui::GetFont();
    float fs = f ? f->LegacySize : 16.0f;
    float charW = f->CalcTextSizeA(fs, FLT_MAX, -1.0f, "M", nullptr, nullptr).x;
    if (m_lines.empty()) return 0;
    RebuildVisCache();   // 需可视行映射 (含折叠)
    int row = (int)(contentY / LineH());
    if (row < 0) return 0;
    if (row >= (int)m_visLine.size()) return m_text.size();
    size_t line = (size_t)m_visLine[row];
    size_t ls = LineStart(line), le = LineEnd(line);
    float x = 0;
    const char* b = m_text.data();
    size_t i = ls;
    while (i < le) {
        size_t i0 = i;
        char c = b[i];
        float w;
        if (c == '\t') {
            size_t col = (size_t)(x / charW);
            w = (float)((col / EditorTabWidth() + 1) * EditorTabWidth()) * charW - x;
            i++;
        } else {
            size_t e = NextChar(i);
            w = f->CalcTextSizeA(fs, FLT_MAX, -1.0f, b + i, b + e, nullptr).x;
            i = e;
        }
        if (x + w * 0.5f >= contentX) return i0;
        x += w;
    }
    return le;
}

// ===================================================================
//  代码折叠
// ===================================================================
void CodeEditor::InvalidateFolds() {
    m_folds.clear();
    m_visDirty = true;
    m_foldCandsDirty = true;
}

bool CodeEditor::IsLineHidden(int line) const {
    for (auto& f : m_folds)
        if (line > f.first && line <= f.second) return true;
    return false;
}

void CodeEditor::ExpandFoldContaining(int line) {
    bool changed = false;
    for (auto it = m_folds.begin(); it != m_folds.end();) {
        if (line > it->first && line <= it->second) { it = m_folds.erase(it); changed = true; }
        else ++it;
    }
    if (changed) m_visDirty = true;
}

void CodeEditor::RebuildVisCache() const {
    if (!m_visDirty) return;
    m_visLine.clear();
    for (size_t li = 0; li < m_lines.size(); li++) {
        bool hid = false;
        for (auto& f : m_folds)
            if ((size_t)f.first < li && li <= (size_t)f.second) { hid = true; break; }
        if (!hid) m_visLine.push_back((int)li);
    }
    m_visDirty = false;
}

int CodeEditor::VisibleRowOfLine(int line) const {
    if (line < 0 || (size_t)line >= m_lines.size() || IsLineHidden(line)) return -1;
    int hiddenAbove = 0;
    for (auto& f : m_folds)
        if ((size_t)f.second < (size_t)line) hiddenAbove += f.second - f.first;
    return line - hiddenAbove;
}

int CodeEditor::LineOfVisualRow(int row) const {
    RebuildVisCache();
    if (m_visLine.empty()) return 0;
    if (row < 0) row = 0;
    if (row >= (int)m_visLine.size()) row = (int)m_visLine.size() - 1;
    return m_visLine[row];
}

float CodeEditor::VisualY(int line) const {
    int r = VisibleRowOfLine(line);
    return (r < 0) ? -1e9f : (float)r * LineH();
}

// 括号配平: 忽略字符串/字符/注释里的花括号, 找到与锚行配对的 "}" 所在行
int CodeEditor::FindFoldEnd(int start) const {
    if (start < 0 || (size_t)start >= m_lines.size()) return -1;
    int depth = 0, braceAt = 0;
    bool inStr = false, inCh = false, inLineC = false, inBlock = false;
    for (size_t li = (size_t)start; li < m_lines.size() && (int)(li - (size_t)start) < 600; li++) {
        size_t ls = LineStart(li), le = LineEnd(li);
        const char* b = m_text.data();
        for (size_t i = ls; i < le; i++) {
            char c = b[i];
            if (inLineC) { continue; }
            if (inStr) {
                if (c == '\\') { i++; continue; }
                if (c == '"') inStr = false;
                continue;
            }
            if (inCh) {
                if (c == '\\') { i++; continue; }
                if (c == '\'') inCh = false;
                continue;
            }
            if (inBlock) {
                if (c == '*' && i + 1 < le && b[i + 1] == '/') { inBlock = false; i++; }
                continue;
            }
            if (c == '/' && i + 1 < le) {
                if (b[i + 1] == '/') { inLineC = true; i++; continue; }
                if (b[i + 1] == '*') { inBlock = true; i++; continue; }
            }
            if (c == '"') { inStr = true; continue; }
            if (c == '\'') { inCh = true; continue; }
            if (c == '{') { depth++; braceAt = 0; }
            else if (c == '}') {
                if (depth > 0) {
                    depth--;
                    if (depth == 0) return (int)li;   // 配平完成: 折到该行
                }
            }
        }
        if (inLineC) inLineC = false;   // 行注释到换行结束
        if (depth == 0 && (int)li > start) break;   // 早期 return 已处理; 保底
    }
    return -1;
}

void CodeEditor::BuildFoldCandidates() const {
    if (!m_foldCandsDirty) return;
    m_foldCands.clear();
    for (size_t li = 0; li < m_lines.size(); li++) {
        size_t ls = LineStart(li), le = LineEnd(li);
        size_t b = ls;
        while (b < le && (m_text[b] == ' ' || m_text[b] == '\t')) b++;
        size_t e = le;
        while (e > b && (m_text[e - 1] == ' ' || m_text[e - 1] == '\t' || m_text[e - 1] == '\r')) e--;
        if (e > b && m_text[e - 1] == '{') {
            // 用 m_lineInComment 排除纯注释块里的伪括号行 (近似)
            bool inCmt = (li < m_lineInComment.size()) && m_lineInComment[li];
            if (!inCmt && FindFoldEnd((int)li) > (int)li + 1)
                m_foldCands.push_back((int)li);
        }
    }
    m_foldCandsDirty = false;
}

void CodeEditor::ToggleFoldAt(int line) {
    if (line < 0 || (size_t)line >= m_lines.size()) return;
    // 已折叠 -> 展开
    for (auto it = m_folds.begin(); it != m_folds.end(); ++it) {
        if (it->first == line) { m_folds.erase(it); m_visDirty = true; return; }
    }
    if (IsLineHidden(line)) return;   // 锚行不能在已折叠区内
    BuildFoldCandidates();
    int end = FindFoldEnd(line);
    if (end <= line + 1) return;
    // 若折叠会盖住光标, 则不允许 (避免光标消失)
    size_t cl = LineOf(m_cursor);
    if ((size_t)line < cl && cl <= (size_t)end) return;
    m_folds.push_back({ line, end });
    std::sort(m_folds.begin(), m_folds.end());
    m_visDirty = true;
}

void CodeEditor::RebuildDecorations() {
    if (!m_decorationDirty&&m_decorationTabWidth==TabWidth()) return;
    m_decorationTabWidth=TabWidth();
    m_decorationDirty = false;
    m_bracketLevel.assign(m_text.size(), -1);
    m_indentColumns.assign(m_lines.size(), 0);
    std::vector<std::pair<char,int>> stack;
    int braces = 0;
    for (size_t line=0; line<m_lines.size(); ++line) {
        size_t start=LineStart(line), end=LineEnd(line), p=start;
        int columns=0;
        while (p<end && (m_text[p]==' ' || m_text[p]=='\t')) {
            columns = m_text[p]=='\t' ? (columns/TabWidth()+1)*TabWidth() : columns+1; ++p;
        }
        m_indentColumns[line] = p == end ? std::max(columns, braces*TabWidth()) : columns;
        EnsureTokens(line);
        for (const auto& token : m_tokCache[line]) {
            if (token.type == TokenKind::String || token.type == TokenKind::Comment || token.type == TokenKind::Preprocessor) continue;
            for (size_t pos=start+token.start; pos<start+token.start+token.len; ++pos) {
                char c=m_text[pos];
                if (strchr("([{",c)) {
                    m_bracketLevel[pos]=(int)stack.size()%4;
                    stack.push_back({c,(int)pos}); if(c=='{') ++braces;
                } else if (strchr(")]}",c)) {
                    char open = c==')' ? '(' : c==']' ? '[' : '{';
                    if (!stack.empty() && stack.back().first==open) {
                        m_bracketLevel[pos]=m_bracketLevel[stack.back().second]; stack.pop_back();
                        if(c=='}') braces=std::max(0,braces-1);
                    } else m_bracketLevel[pos]=-2;
                }
            }
        }
    }
}

ImU32 CodeEditor::SyntaxColor(const Token& token, size_t start, size_t end) const {
    if (!g_app.settings.syntaxHighlight) return g_app.EditorColorValue(EditorColor::Text);
    EditorColor role=EditorColor::Text;
    size_t pos=start+token.start;
    std::string word=m_text.substr(pos,token.len);
    switch(token.type) {
    case TokenKind::Keyword: role=BuiltinTypeSet().count(word) ? EditorColor::TypeKeyword : EditorColor::Keyword; break;
    case TokenKind::TypeName: role=EditorColor::Class; break;
    case TokenKind::Comment: role=EditorColor::Comment; break;
    case TokenKind::Preprocessor: role=EditorColor::Preprocessor; break;
    case TokenKind::String: role=!word.empty() && word[0]=='\'' ? EditorColor::Character : EditorColor::String; break;
    case TokenKind::Number: {
        bool hex=word.size()>2 && word[0]=='0' && (word[1]=='x'||word[1]=='X');
        bool floating=word.find('.')!=std::string::npos || word.find_first_of(hex ? "pP" : "eE")!=std::string::npos;
        role=floating ? EditorColor::Float : hex ? EditorColor::Hex :
             word.size()>1 && word[0]=='0' && word[1]>='0' && word[1]<='7' ? EditorColor::Octal : EditorColor::Integer;
        break;
    }
    default:
        if(word.empty()) break;
        if (word[0]==' ' || word[0]=='\t') role=EditorColor::Whitespace;
        else if (!IsIdentStart(word[0])) {
            role=(word[0]=='@'||word[0]=='`'||(unsigned char)word[0]<32) ? EditorColor::Illegal : EditorColor::Symbol;
        } else {
            role=EditorColor::Identifier;
            size_t next=pos+token.len; while(next<end && (m_text[next]==' '||m_text[next]=='\t')) ++next;
            if (m_code.FindType(word)) role=EditorColor::Class;
            else if (next<end && m_text[next]=='(') role=EditorColor::Function;
            else {
                for (auto& symbol:m_code.Globals()) if(symbol.name==word) {
                    if(symbol.kind==CSymKind::Namespace) role=EditorColor::Namespace;
                    else if(symbol.kind==CSymKind::Function) role=EditorColor::Function;
                    else if(symbol.kind==CSymKind::Variable) role=EditorColor::Global;
                }
                for(auto& symbol:m_code.Locals()) if(symbol.name==word && symbol.declPos<=pos && symbol.scopeStart<=pos &&
                    (!symbol.scopeEnd||pos<=symbol.scopeEnd)) role=EditorColor::Local;
                if (role==EditorColor::Identifier && next+1<end && m_text[next]==':'&&m_text[next+1]==':') role=EditorColor::Namespace;
                if (role==EditorColor::Identifier && pos>start && (m_text[pos-1]=='.'||m_text[pos-1]=='>' ||
                    (pos>=start+2 && m_text[pos-1]==':' && m_text[pos-2]==':'))) role=EditorColor::Variable;
            }
        }
        break;
    }
    return g_app.EditorColorValue(role);
}

// ===================================================================
//  Token 化 (语法高亮)
// ===================================================================
std::vector<Token> CodeEditor::TokenizeLine(size_t line, bool& inBlockComment) const {
    std::vector<Token> toks;
    size_t ls = LineStart(line), le = LineEnd(line);
    const char* b = m_text.data();
    size_t i = ls;

    // 预处理指令: 行首(可带空白)为 #
    if (!inBlockComment) {
        size_t j = ls;
        while (j < le && (b[j] == ' ' || b[j] == '\t')) j++;
        if (j < le && b[j] == '#') {
            toks.push_back({ 0, le - ls, TokenKind::Preprocessor });
            return toks;
        }
    }

    while (i < le) {
        char c = b[i];
        if (inBlockComment) {
            size_t start = i;
            while (i < le) {
                if (b[i] == '*' && i + 1 < le && b[i+1] == '/') { i += 2; inBlockComment = false; break; }
                ++i;
            }
            toks.push_back({start-ls, i-start, TokenKind::Comment});
            continue;
        }
        // 行注释
        if (c == '/' && i + 1 < le && b[i + 1] == '/') {
            toks.push_back({ i - ls, le - i, TokenKind::Comment });
            break;
        }
        // 块注释
        if (c == '/' && i + 1 < le && b[i + 1] == '*') {
            inBlockComment = true;
            size_t s = i;
            i += 2;
            while (i < le) {
                if (b[i] == '*' && i + 1 < le && b[i + 1] == '/') { inBlockComment = false; i += 2; break; }
                i++;
            }
            toks.push_back({ s - ls, i - s, TokenKind::Comment });
            continue;
        }
        // 字符串 / 字符
        if (c == '"' || c == '\'') {
            char q = c;
            size_t s = i;
            i++;
            while (i < le) {
                if (b[i] == '\\' && i + 1 < le) { i += 2; continue; }
                if (b[i] == q) { i++; break; }
                i++;
            }
            toks.push_back({ s - ls, i - s, TokenKind::String });
            continue;
        }
        // 数字
        if (std::isdigit((unsigned char)c) || (c == '.' && i + 1 < le && std::isdigit((unsigned char)b[i + 1]))) {
            size_t s = i;
            if (c == '0' && i + 1 < le && (b[i + 1] == 'x' || b[i + 1] == 'X' ||
                                           b[i + 1] == 'b' || b[i + 1] == 'B')) {
                i += 2;
                while (i < le && (std::isxdigit((unsigned char)b[i]) || b[i] == '_' || b[i] == '.')) i++;
            } else {
                while (i < le && (std::isdigit((unsigned char)b[i]) || b[i] == '.' || b[i] == '_')) i++;
                // 仅当 e/E 后确实跟数字(可带一个正负号)才算指数, 避免把 1+2 / 1-2 吞成一个 Number
                if (i < le && (b[i] == 'e' || b[i] == 'E')) {
                    size_t j = i + 1;
                    if (j < le && (b[j] == '+' || b[j] == '-')) j++;
                    if (j < le && std::isdigit((unsigned char)b[j])) {
                        i = j;
                        while (i < le && (std::isdigit((unsigned char)b[i]) || b[i] == '_')) i++;
                    }
                }
            }
            while (i < le && (std::isalpha((unsigned char)b[i]) || b[i] == '_')) i++; // 后缀
            toks.push_back({ s - ls, i - s, TokenKind::Number });
            continue;
        }
        // 标识符 / 关键字
        if (IsIdentStart(c)) {
            size_t s = i;
            while (i < le && IsIdentChar(b[i])) i++;
            std::string word = m_text.substr(s, i - s);
            TokenKind t = TokenKind::Default;
            if (KeywordSet().count(word)) t = TokenKind::Keyword;
            else if (TypeNameSet().count(word)) t = TokenKind::TypeName;
            toks.push_back({ s - ls, i - s, t });
            continue;
        }
        size_t start = i++;
        if (c == ' ' || c == '\t') while (i < le && (b[i] == ' ' || b[i] == '\t')) ++i;
        toks.push_back({start-ls, i-start, TokenKind::Default});
    }
    return toks;
}

// ===================================================================
//  补全辅助: 模糊匹配 / 缩写 / 类型归一化 / 成员数据库
// ===================================================================
static std::string CompToLower(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}
// 子序列模糊匹配: "pb" -> push_back, "vr" -> vector
static bool FuzzySubseq(const std::string& cand, const std::string& prefix) {
    std::string C = CompToLower(cand), P = CompToLower(prefix);
    size_t j = 0;
    for (size_t i = 0; i < C.size() && j < P.size(); i++)
        if (C[i] == P[j]) j++;
    return j == P.size();
}
// 命名首字母缩写: push_back -> pb, HelloWorld -> hw
static std::string Initials(const std::string& s) {
    std::string r;
    bool nw = true;
    for (char c : s) {
        if (c == '_' || c == '.') { nw = true; continue; }
        if (nw) { r += (char)tolower((unsigned char)c); nw = false; }
        else if (isupper((unsigned char)c)) r += (char)tolower((unsigned char)c);
    }
    return r;
}
// 大小写不敏感前缀
static bool CIPrefix(const std::string& w, const std::string& p) {
    if (w.size() < p.size()) return false;
    for (size_t i = 0; i < p.size(); i++)
        if (tolower((unsigned char)w[i]) != tolower((unsigned char)p[i])) return false;
    return true;
}
// 截断过长签名 (补全列表/参数提示显示用)
static std::string TrimSig(const std::string& sig, size_t maxLen = 64) {
    if (sig.size() <= maxLen) return sig;
    return sig.substr(0, maxLen - 3) + "...";
}

// 预小写串上的前缀 / 子序列匹配 (候选池缓存用, 免去每次比较的大小写转换分配)
static bool CIPrefixLow(const std::string& wLow, const std::string& pLow) {
    if (wLow.size() < pLow.size()) return false;
    for (size_t i = 0; i < pLow.size(); i++)
        if (wLow[i] != pLow[i]) return false;
    return true;
}
static bool FuzzySubseqLow(const std::string& wLow, const std::string& pLow) {
    size_t j = 0;
    for (size_t i = 0; i < wLow.size() && j < pLow.size(); i++)
        if (wLow[i] == pLow[j]) j++;
    return j == pLow.size();
}
// 候选池打分: 与 ScoreOf 完全一致, 只是复用预计算的小写/缩写串
static int ScorePool(const CodeEditor::CompPoolEntry& e, const std::string& p, const std::string& pLow) {
    if (p.empty()) return 100 - (int)e.label.size();
    if (e.label == p) return 5000;
    if (CIPrefixLow(e.lower, pLow)) return 3000 - (int)e.label.size() * 2;
    if (FuzzySubseqLow(e.lower, pLow)) {
        int sc = 1000 - (int)e.label.size();
        if (CIPrefixLow(e.initials, pLow)) sc += 800;
        return sc;
    }
    return -1;
}
// 静态候选池: 关键字 + 类型 + 常用名 (代码符号由 CodeModel 动态提供)
void CodeEditor::EnsureWordPool() {
    if (!m_wordPoolDirty) return;
    m_wordPoolDirty = false;
    m_wordPool.clear();

    if (m_identsDirty) CollectIdents();     // 提供词频加成 (常用的靠前)
    std::unordered_map<std::string, int> index;
    auto offer = [&](const std::string& w, const char* kind, const std::string& hint, bool func, int boost) {
        if (index.find(w) != index.end()) return;
        auto f = m_identFreq.find(w);
        int base = boost + SymUsageBonus(w)
                 + (f != m_identFreq.end() ? std::min(f->second, 20) * 15 : 0);
        CompPoolEntry e;
        e.label = w; e.kind = kind; e.hint = hint; e.func = func; e.doc = false; e.base = base;
        e.lower = CompToLower(w);
        e.initials = Initials(w);
        index.emplace(w, (int)m_wordPool.size());
        m_wordPool.push_back(std::move(e));
    };

    for (auto& k : KeywordSet())     offer(k, "关键字", "", false, 40);
    for (auto& k : BuiltinTypeSet()) offer(k, "类型",   "", false, 60);
}

// 翻译单元包含当前文档，缓存必须随文档、路径及编译器变化失效。
// 仿小熊猫 CppPreprocessor: 头文件里的类型/函数/成员都会进入符号表, 未 include 的则不会出现。
void CodeEditor::EnsureHeaderModel() const {
    std::vector<std::string> incs = SymbolIndex::I().ExtractIncludeNames(m_text);
    uint64_t sig = 1469598103934665603ull;
    auto hashText = [&](const std::string& value) {
        for (unsigned char c : value) { sig ^= c; sig *= 1099511628211ull; }
        sig ^= 0x1f; sig *= 1099511628211ull;
    };
    hashText(m_text);
    hashText(utils::WToU8(m_path));
    hashText(g_app.settings.gppPath);
    for (auto& n : incs) {
        for (unsigned char c : n) { sig ^= c; sig *= 1099511628211ull; }
        sig ^= 0x1f; sig *= 1099511628211ull;
    }
    if (m_hdrValid && sig == m_hdrSig) return;
    m_hdrSig = sig;
    m_hdrValid = true;
    m_hdr.SetSystemSource(true);
    if (incs.empty() || !SymbolIndex::I().HasIncludeDirs()) {
        m_hdrUnit = SymbolIndex::HeaderUnit{};
        m_hdr.Rebuild("");
        return;
    }
    m_hdrUnit = SymbolIndex::I().BuildHeaderUnit(m_text, m_path);
    m_hdr.Rebuild(m_hdrUnit.text);
}

// -------------------------------------------------------------------
//  #include 文件补全 (本地目录 / include 搜索路径 / 目录下钻 / 使用频率)
// -------------------------------------------------------------------

// 前置声明: 以下两表在文件后部定义, 供无可用目录时的兜底提示使用
static const std::vector<std::pair<std::string, const char*>>& HeaderList();
static int ScoreOf(const std::string& w, const std::string& p);

// 列出一个目录中的候选: 子目录 + 头文件 (h/hpp/hh/hxx 或无扩展名, 同 RedPanda 思路)
static void ScanIncludeDir(const std::wstring& dirW, int type,
                           std::vector<CodeEditor::FileCompEntry>& out,
                           std::unordered_set<std::string>& seen) {
    std::error_code ec;
    std::filesystem::path dir(dirW);
    if (!std::filesystem::is_directory(dir, ec)) return;
    std::vector<std::filesystem::directory_entry> entries;
    std::filesystem::directory_iterator it(dir, std::filesystem::directory_options::skip_permission_denied, ec);
    for (; !ec && it != std::filesystem::directory_iterator(); it.increment(ec))
        entries.push_back(*it);
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
        return a.path().filename().wstring() < b.path().filename().wstring();
    });
    for (auto& de : entries) {
        std::wstring name = de.path().filename().wstring();
        if (name.empty() || name[0] == L'.') continue;      // 隐藏项
        std::error_code ec2;
        bool isDir = de.is_directory(ec2);
        if (!isDir) {
            std::wstring ext = de.path().extension().wstring();
            for (auto& c : ext) c = (wchar_t)towlower(c);
            if (ext != L".h" && ext != L".hpp" && ext != L".hh" && ext != L".hxx" && !ext.empty())
                continue;                                   // 只收头文件/无扩展名文件
        }
        std::string nameU8 = utils::WToU8(name);
        if (!seen.insert(nameU8).second) continue;           // 同名去重 (本地优先)
        CodeEditor::FileCompEntry e;
        e.name = nameU8;
        e.fullPath = de.path().wstring();
        e.type = type;
        e.isDir = isDir;
        out.push_back(std::move(e));
    }
}

// 使用频率持久化 (dataDir\comp_usage.json): fullPath -> 次数
void CodeEditor::LoadFileUsage() {
    m_fileUsageLoaded = true;
    if (g_app.dataDir.empty()) return;
    std::string s;
    if (!utils::ReadFileU8(g_app.dataDir + L"\\comp_usage.json", s) || s.empty()) return;
    try {
        auto j = nlohmann::json::parse(s);
        if (!j.is_object()) return;
        for (auto it = j.begin(); it != j.end(); ++it)
            m_fileUsage[utils::U8ToW(it.key())] = it.value().get<int>();
    } catch (...) {}
}

void CodeEditor::SaveFileUsage() {
    if (g_app.dataDir.empty()) return;
    try {
        nlohmann::json j = nlohmann::json::object();
        for (auto& kv : m_fileUsage) j[utils::WToU8(kv.first)] = kv.second;
        utils::WriteFileU8(g_app.dataDir + L"\\comp_usage.json", j.dump(2));
    } catch (...) {}
}

void CodeEditor::BumpFileUsage(const std::wstring& fullPath) {
    if (fullPath.empty()) return;
    if (!m_fileUsageLoaded) LoadFileUsage();
    m_fileUsage[fullPath]++;
    SaveFileUsage();
}

// 符号使用频率 (dataDir\comp_sym_usage.json): 名字 -> 次数; 供补全排序 "常用的靠前"
void CodeEditor::LoadSymUsage() {
    m_symUsageLoaded = true;
    if (g_app.dataDir.empty()) return;
    std::string s;
    if (!utils::ReadFileU8(g_app.dataDir + L"\\comp_sym_usage.json", s) || s.empty()) return;
    try {
        auto j = nlohmann::json::parse(s);
        if (!j.is_object()) return;
        for (auto it = j.begin(); it != j.end(); ++it)
            m_symUsage[it.key()] = it.value().get<int>();
    } catch (...) {}
}

void CodeEditor::SaveSymUsage() {
    if (g_app.dataDir.empty()) return;
    try {
        nlohmann::json j = nlohmann::json::object();
        for (auto& kv : m_symUsage) j[kv.first] = kv.second;
        utils::WriteFileU8(g_app.dataDir + L"\\comp_sym_usage.json", j.dump(2));
    } catch (...) {}
}

void CodeEditor::BumpSymUsage(const std::string& name) {
    if (name.empty()) return;
    if (!m_symUsageLoaded) LoadSymUsage();
    m_symUsage[name]++;
    SaveSymUsage();
}

int CodeEditor::SymUsageBonus(const std::string& name) {
    if (name.empty()) return 0;
    if (!m_symUsageLoaded) LoadSymUsage();
    auto it = m_symUsage.find(name);
    if (it == m_symUsage.end()) return 0;
    return std::min(it->second, 10) * 8;   // 最高 +80
}

// 光标是否位于未闭合的 #include "路径 或 <路径 内; pathStart 为引号/尖括号之后的位置
bool CodeEditor::InIncludeCompletion(size_t& pathStart, bool& isQuote) const {
    if (m_text.empty() || m_cursor > m_text.size()) return false;
    size_t line = LineOf(m_cursor);
    if (line >= m_lines.size()) return false;
    if (line < m_lineInComment.size() && m_lineInComment[line]) return false;   // /* */ 内
    size_t ls = LineStart(line);
    size_t fb = ls;
    while (fb < m_text.size() && (m_text[fb] == ' ' || m_text[fb] == '\t')) fb++;
    if (fb + 1 < m_text.size() && m_text[fb] == '/' && m_text[fb + 1] == '/') return false; // // 注释

    std::string before = m_text.substr(ls, m_cursor - ls);
    int q = -1; char qc = 0;
    for (int k = (int)before.size() - 1; k >= 0; k--) {
        char c = before[k];
        if (c == '"') { q = k; qc = '"'; break; }
        if (c == '<') { q = k; qc = '<'; break; }
        if (c == '>' || c == ';' || c == '(' || c == ')' || c == '{' || c == '}') return false;
    }
    if (q < 0) return false;
    // 头部必须是 #include (允许 # 与 include 之间空白)
    std::string head = before.substr(0, (size_t)q);
    size_t he = head.find_last_not_of(" \t");
    if (he == std::string::npos) return false;
    head.resize(he + 1);
    static const std::string kw = "include";
    if (head.size() < kw.size() || head.compare(head.size() - kw.size(), kw.size(), kw) != 0) return false;
    std::string pre = head.substr(0, head.size() - kw.size());
    size_t pe = pre.find_last_not_of(" \t");
    if (pe == std::string::npos || pre[pe] != '#') return false;
    if (head.find('"') != std::string::npos) return false;   // 引号已闭合
    pathStart = ls + (size_t)q + 1;
    isQuote = (qc == '"');
    return true;
}

// 补全前缀起点: include 上下文下包含 '/'、'\'、'.'、'-'、'+' 等路径字符
size_t CodeEditor::CompPrefixStart(bool& isInclude, bool& isQuote) const {
    size_t hs = 0; bool q = false;
    if (InIncludeCompletion(hs, q)) { isInclude = true; isQuote = q; return hs; }
    isInclude = false;
    size_t p = m_cursor;
    while (p > 0 && IsIdentChar(m_text[PrevChar(p)])) p = PrevChar(p);
    return p;
}

void CodeEditor::GatherHeaderCandidates(const std::string& phrase, bool localQuote,
                                        std::vector<CompItem>& out) {
    if (!m_fileUsageLoaded) LoadFileUsage();

    size_t sep = phrase.find_last_of("/\\");
    std::string dirPart = (sep == std::string::npos) ? std::string() : phrase.substr(0, sep + 1);
    std::string base    = (sep == std::string::npos) ? phrase : phrase.substr(sep + 1);

    // 目录级缓存: 仅当"来源 + 目录前缀 + 当前文件"变化时才重新扫盘 (输入文件名时只过滤)
    std::string key = std::string(localQuote ? "L" : "S") + "|" + dirPart + "|" +
                      std::to_string(m_uid) + "|" + utils::WToU8(m_path);
    if (key != m_fileCompKey) {
        m_fileCompKey = key;
        m_fileCompCache.clear();
        std::unordered_set<std::string> seen;
        std::vector<std::pair<std::wstring,int>> roots;
        if (localQuote && HasPath()) roots.push_back({ utils::DirName(m_path), 0 });   // "..." 先查本地
        for (auto& d : SymbolIndex::I().IncludeDirs()) roots.push_back({ d, 1 });      // 再查 include 路径
        for (auto& r : roots) {
            std::wstring dir = r.first;
            if (!dirPart.empty()) dir += L"\\" + utils::U8ToW(dirPart);
            ScanIncludeDir(dir, r.second, m_fileCompCache, seen);
        }
    }

    struct Cand { int usage; int type; const FileCompEntry* e; };
    std::vector<Cand> cands;
    for (auto& e : m_fileCompCache) {
        if (!base.empty() && !CIPrefix(e.name, base)) continue;   // 按最后一段做大小写不敏感前缀
        auto f = m_fileUsage.find(e.fullPath);
        cands.push_back({ f != m_fileUsage.end() ? f->second : 0, e.type, &e });
    }
    std::stable_sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
        if (a.usage != b.usage) return a.usage > b.usage;          // 使用频率高优先
        if (a.type != b.type)   return a.type < b.type;            // 本地优先于系统
        int c = _stricmp(a.e->name.c_str(), b.e->name.c_str());
        if (c != 0) return c < 0;
        return false;
    });

    if (cands.empty()) {
        // 兜底: 没有可用目录 (如未配置 g++) 时, 仍按内置标准头文件表提示
        for (auto& hd : HeaderList()) {
            int sc = ScoreOf(hd.first, base);
            if (sc < 0) continue;
            CompItem it;
            it.label = hd.first;
            it.kind = "头文件";
            it.hint = hd.second;
            out.push_back(std::move(it));
        }
        return;
    }
    out.reserve(cands.size());
    for (auto& c : cands) {
        CompItem it;
        it.label  = c.e->isDir ? (c.e->name + "/") : c.e->name;
        it.kind   = c.e->isDir ? "文件夹" : (c.e->type == 0 ? "本地头文件" : "系统头文件");
        it.hint   = c.e->isDir ? "目录" : "";
        it.func   = false;
        it.fullPath = c.e->fullPath;
        it.isDir  = c.e->isDir;
        out.push_back(std::move(it));
    }
}

uint64_t CodeEditor::s_uidCounter = 0;
// 取 pos 处 (或其前) 的标识符 [a-zA-Z0-9_]+
std::string CodeEditor::IdentAt(size_t pos) {
    if (m_text.empty()) return "";
    if (pos > m_text.size()) pos = m_text.size();
    auto isId = [](char c) { return isalnum((unsigned char)c) || c == '_'; };
    size_t a = pos, b = pos;
    while (a > 0 && isId(m_text[a - 1])) a--;
    while (b < m_text.size() && isId(m_text[b])) b++;
    if (a >= b) return "";
    return m_text.substr(a, b - a);
}

// 跳转定义: 在所有打开文档中搜索 ident 的定义行
//   匹配: class/struct/enum <ident>  |  <ident>::  |  <ident>( (函数定义)
//   顺序: 当前文档优先, 其余标签按打开顺序
bool CodeEditor::GotoDefinition(bool useLanguage) {
    if(useLanguage && language::Definition(*this))return true;
    // -1) 光标位于 #include <...> / "..." 的文件名内: 打开该头文件 (而非把名字当类型解析)
    {
        size_t line = LineOf(m_cursor);
        size_t ls = LineStart(line), le = LineEnd(line);
        size_t p = ls;
        while (p < le && (m_text[p] == ' ' || m_text[p] == '\t')) p++;
        if (p < le && m_text[p] == '#') {
            size_t q = p + 1;
            while (q < le && (m_text[q] == ' ' || m_text[q] == '\t')) q++;
            if (q + 7 <= le && m_text.compare(q, 7, "include") == 0) {
                q += 7;
                while (q < le && (m_text[q] == ' ' || m_text[q] == '\t')) q++;
                if (q < le && (m_text[q] == '<' || m_text[q] == '"')) {
                    bool quote = (m_text[q] == '"');
                    char close = quote ? '"' : '>';
                    size_t c = q + 1;
                    while (c < le && m_text[c] != close) c++;
                    if (c < le && c > q + 1 && m_cursor >= q + 1 && m_cursor <= c) {
                        std::string hname = m_text.substr(q + 1, c - q - 1);
                        std::wstring wname = utils::U8ToW(hname);
                        for (auto& ch2 : wname) if (ch2 == L'/') ch2 = L'\\';
                        std::error_code ec;
                        std::wstring path;
                        if (quote && HasPath()) {
                            std::wstring lp = utils::DirName(m_path) + L"\\" + wname;
                            if (std::filesystem::is_regular_file(std::filesystem::path(lp), ec)) path = lp;
                        }
                        if (path.empty())
                            for (auto& d : SymbolIndex::I().IncludeDirs()) {
                                std::wstring tp = d + L"\\" + wname;
                                if (std::filesystem::is_regular_file(std::filesystem::path(tp), ec)) { path = tp; break; }
                            }
                        if (!path.empty()) {
                            g_app.console.Append("[跳转] 打开头文件 \"" + hname + "\" -> " +
                                                 utils::WToU8(path) + "\n", IM_COL32(140, 200, 255, 255));
                            g_app.OpenAtFile(utils::WToU8(path), 1);
                            return true;
                        }
                        g_app.console.Append("[跳转] 未找到头文件 \"" + hname + "\" (请检查 include 路径)\n",
                                             IM_COL32(255, 180, 90, 255));
                        return false;
                    }
                }
            }
        }
    }

    std::string id = IdentAt(m_cursor);
    if (id.empty()) {
        g_app.console.Append("[跳转] 光标处无标识符\n", IM_COL32(255, 180, 90, 255));
        return false;
    }
    if (id == "int" || id == "char" || id == "void" || id == "bool" ||
        id == "float" || id == "double" || id == "auto" || id == "if" ||
        id == "for" || id == "while" || id == "return" || id == "switch" ||
        id == "else" || id == "case" || id == "break" || id == "continue" ||
        id == "const" || id == "static" || id == "new" || id == "delete" ||
        id == "class" || id == "struct" || id == "enum" || id == "namespace" ||
        id == "using" || id == "template" || id == "typename" || id == "public" ||
        id == "private" || id == "include") {
        g_app.console.Append("[跳转] \"" + id + "\" 是关键字, 无定义\n", IM_COL32(255, 180, 90, 255));
        return false;
    }

    // 0) 语义模型: 精确定位声明 (局部/参数/全局/类型/类成员), 头文件模型补足库符号
    EnsureCodeModel();
    auto jumpInDoc = [&](size_t dp) -> bool {
        if (dp >= m_text.size()) return false;
        int line = (int)LineOf(dp) + 1;
        std::string fname = HasPath() ? utils::WToU8(m_path) : m_name;
        g_app.console.Append("[跳转] \"" + id + "\" -> " + fname + ":" + std::to_string(line) + "\n",
                             IM_COL32(140, 200, 255, 255));
        GotoLine(line);
        return true;
    };
    auto jumpToFile = [&](const std::wstring& file, int line) -> bool {
        if (file.empty() || line <= 0) return false;
        if (_wcsicmp(file.c_str(), m_path.c_str()) == 0) {
            g_app.console.Append("[跳转] \"" + id + "\" -> " + m_name + ":" + std::to_string(line) + "\n",
                                 IM_COL32(140, 200, 255, 255));
            GotoLine(line);
        } else {
            g_app.console.Append("[跳转] \"" + id + "\" -> " + utils::WToU8(file) + ":" +
                                 std::to_string(line) + "\n", IM_COL32(140, 200, 255, 255));
            g_app.OpenAtFile(utils::WToU8(file), line);
        }
        return true;
    };

    // 0a) 当前文档符号
    {
        size_t dp = 0;
        if (m_code.FindDeclPos(id, m_cursor, dp)) return jumpInDoc(dp);
    }
    // 0b) 成员访问 obj.member / obj->member: 先解析对象类型, 再定位成员声明
    std::string memberType;
    {
        size_t ia = m_cursor;
        while (ia > 0 && IsIdentChar(m_text[ia - 1])) ia--;
        if (ia >= 1 && (m_text[ia - 1] == '.' ||
                        (ia >= 2 && m_text[ia - 1] == '>' && m_text[ia - 2] == '-'))) {
            size_t dot = (m_text[ia - 1] == '.') ? ia - 1 : ia - 2;
            size_t p = dot;
            int dp2 = 0, db = 0;
            while (p > 0) {
                size_t r = PrevChar(p);
                char c = m_text[r];
                if (c == ')') { dp2++; p = r; continue; }
                if (c == ']') { db++; p = r; continue; }
                if (c == '(') { if (dp2 > 0) { dp2--; p = r; continue; } break; }
                if (c == '[') { if (db > 0) { db--; p = r; continue; } break; }
                if (dp2 == 0 && db == 0) {
                    if (IsIdentChar(c) || c == '.' || c == ':' || c == '>' || c == '-') { p = r; continue; }
                    break;
                }
                p = r;
            }
            memberType = m_code.EvalExprType(m_text.substr(p, dot - p), m_cursor);
            if (!memberType.empty()) {
                size_t dp = 0;
                if (m_code.FindMemberDeclPos(memberType, id, dp)) return jumpInDoc(dp);
            }
        }
    }
    // 0c) 头文件模型: #include 的头文件里解析出的符号/成员 (如 std::cout -> <iostream>)
    if (CompilerAvail()) {
        EnsureHeaderModel();
        size_t tdp = 0;
        bool found = false;
        if (!memberType.empty() && m_hdr.FindMemberDeclPos(memberType, id, tdp)) found = true;
        else if (m_hdr.FindDeclPos(id, 0, tdp)) found = true;
        if (found) {
            std::wstring file; int line = 0;
            if (m_hdrUnit.Locate(tdp, file, line)) return jumpToFile(file, line);
        }
    }

    auto matchLine = [&](const std::string& line) -> bool {
        size_t p = line.find(id);
        while (p != std::string::npos) {
            bool lOk = (p == 0) || !(isalnum((unsigned char)line[p - 1]) || line[p - 1] == '_');
            size_t e = p + id.size();
            bool rOk = true;
            if (e < line.size() && (isalnum((unsigned char)line[e]) || line[e] == '_')) rOk = false;
            if (lOk && rOk) {
                // 形态 1: class/struct/enum <id>
                size_t k = line.find_last_not_of(" \t", p == 0 ? 0 : p - 1);
                std::string head = (k == std::string::npos || p == 0) ? "" : line.substr(0, p);
                auto trim = [](std::string s2) {
                    while (!s2.empty() && (s2.back() == ' ' || s2.back() == '\t')) s2.pop_back();
                    return s2;
                };
                std::string h = trim(head);
                if (h == "class" || h == "struct" || h == "enum") return true;
                // 形态 2: <id>::  (类外成员定义)
                if (line.compare(e, 2, "::") == 0) return true;
                // 形态 3: <id>( 且该行不像调用语句 (排除 foo(); 调用)
                if (e < line.size() && line[e] == '(') {
                    int depth = 0;
                    size_t q = e;
                    bool closed = false;
                    for (; q < line.size(); q++) {
                        if (line[q] == '(') depth++;
                        else if (line[q] == ')') { depth--; if (depth == 0) { closed = true; break; } }
                    }
                    if (!closed) return true;                    // 跨行定义
                    size_t r = line.find_first_not_of(" \t", q + 1);
                    if (r == std::string::npos) return true;     // 行尾 ')', 函数体在下一行
                    if (line[r] == '{') return true;             // ") {"
                    if (line.compare(r, 5, "const") == 0) return true;
                    if (line.find(';', q) == std::string::npos) return true;
                    return false;                                // 含分号: 像调用语句
                }
            }
            p = line.find(id, p + 1);
        }
        return false;
    };

    CodeEditor* hitEd = nullptr;
    int hitLine = -1;
    std::vector<CodeEditor*> order{ this };
    for (auto& ed : g_app.editors)
        if (ed.get() != this) order.push_back(ed.get());
    for (auto* ed : order) {
        if (!ed || ed->m_text.empty()) continue;
        size_t ls = 0;
        int li = 0;
        while (ls < ed->m_text.size()) {
            size_t le = ed->m_text.find('\n', ls);
            if (le == std::string::npos) le = ed->m_text.size();
            std::string line = ed->m_text.substr(ls, le - ls);
            // 跳过纯注释/字符串干扰有限, 简单过滤行首 //
            std::string t = line;
            size_t f = t.find_first_not_of(" \t");
            if (!(f != std::string::npos && t.compare(f, 2, "//") == 0) && matchLine(t)) {
                hitEd = ed;
                hitLine = li + 1;
                break;
            }
            ls = le + 1;
            li++;
        }
        if (hitEd) break;
    }
    if (!hitEd) {
        g_app.console.Append("[跳转] 未找到 \"" + id + "\" 的定义 (已扫描打开的 " +
                             std::to_string(g_app.editors.size()) + " 个标签)\n",
                             IM_COL32(255, 120, 120, 255));
        return false;
    }
    if (hitEd != this) {
        // 切换到目标标签
        for (int k = 0; k < (int)g_app.editors.size(); k++)
            if (g_app.editors[k].get() == hitEd) { g_app.active = k; break; }
        g_app.SyncDiagPanel();
    }
    hitEd->GotoLine(hitLine);
    std::string fname = hitEd->HasPath() ? utils::WToU8(hitEd->m_path) : hitEd->m_name;
    g_app.console.Append("[跳转] \"" + id + "\" -> " + fname + ":" + std::to_string(hitLine) + "\n",
                         IM_COL32(140, 200, 255, 255));
    return true;
}
// 候选打分: <0 不入选
static int ScoreOf(const std::string& w, const std::string& p) {
    if (p.empty()) return 100 - (int)w.size();          // 无前缀(成员列表): 越短越靠前
    if (w == p) return 5000;
    if (CIPrefix(w, p)) return 3000 - (int)w.size() * 2;
    if (FuzzySubseq(w, p)) {
        int sc = 1000 - (int)w.size();
        std::string ini = Initials(w);
        if (CIPrefix(ini, p)) sc += 800;                 // 缩写命中加成
        return sc;
    }
    return -1;
}
using CI = CodeEditor::CompItem;

// 成员数据库: 归一化类型名 -> 成员列表
static const std::unordered_map<std::string, std::vector<CI>>& MemberDB() {
    static const std::unordered_map<std::string, std::vector<CI>> db = [] {
        auto F = [](const char* l, const char* h = "") { return CI{ l, "函数", h, true }; };
        auto V = [](const char* l, const char* h = "") { return CI{ l, "成员", h, false }; };
        std::unordered_map<std::string, std::vector<CI>> m;
        m["string"] = m["wstring"] = {
            F("c_str", "-> const char*"), F("data"), F("length"), F("size"), F("empty"),
            F("clear"), F("append"), F("substr", "(pos,len)"), F("find"), F("rfind"),
            F("replace"), F("erase"), F("insert"), F("compare"), F("begin"), F("end"),
            F("push_back"), F("pop_back"), F("at"), F("resize"),
            F("find_first_of"), F("find_last_of") };
        m["vector"] = {
            F("push_back", "尾插"), F("emplace_back", "原地构造"), F("pop_back"), F("size"),
            F("empty"), F("clear"), F("at", "带边界检查"), F("front"), F("back"),
            F("begin"), F("end"), F("rbegin"), F("rend"), F("insert"), F("erase"),
            F("resize"), F("reserve"), F("shrink_to_fit"), F("swap"), F("data") };
        m["map"] = m["multimap"] = {
            F("insert"), F("emplace"), F("erase"), F("find"), F("count"), F("at"),
            F("contains", "C++20"), F("lower_bound"), F("upper_bound"),
            F("begin"), F("end"), F("size"), F("empty"), F("clear"), F("swap") };
        m["unordered_map"] = m["map"];
        m["set"] = m["multiset"] = m["unordered_set"] = {
            F("insert"), F("erase"), F("find"), F("count"), F("contains", "C++20"),
            F("lower_bound"), F("upper_bound"), F("begin"), F("end"),
            F("size"), F("empty"), F("clear") };
        m["deque"] = {
            F("push_back"), F("push_front"), F("pop_back"), F("pop_front"), F("size"),
            F("empty"), F("at"), F("front"), F("back"), F("begin"), F("end"),
            F("insert"), F("erase"), F("resize"), F("clear") };
        m["list"] = {
            F("push_back"), F("push_front"), F("pop_back"), F("pop_front"), F("size"),
            F("empty"), F("front"), F("back"), F("sort"), F("unique"), F("reverse"),
            F("merge"), F("remove"), F("splice"), F("begin"), F("end"), F("insert"), F("erase"), F("clear") };
        m["stack"] = { F("push"), F("pop"), F("top"), F("size"), F("empty"), F("swap") };
        m["queue"] = m["priority_queue"] = { F("push"), F("pop"), F("front"), F("back"), F("top"), F("size"), F("empty"), F("swap") };
        m["pair"] = { V("first", "第一成员"), V("second", "第二成员") };
        m["ostream"] = { F("precision"), F("width"), F("put"), F("write"), F("flush"), F("fill"), F("flags"), F("setf") };
        m["istream"] = { F("get"), F("getline"), F("read"), F("ignore"), F("peek"), F("gcount"), F("sync"), F("seekg"), F("tellg") };
        m["fstream"] = m["ifstream"] = m["ofstream"] = {
            F("open"), F("close"), F("is_open"), F("read"), F("write"), F("getline"),
            F("seekg"), F("tellg"), F("seekp"), F("tellp"), F("flush"),
            F("eof"), F("fail"), F("good"), F("bad") };
        m["stringstream"] = m["istringstream"] = m["ostringstream"] = { F("str", "取/设内容"), F("flush") };
        m["thread"] = { F("join", "等待结束"), F("detach", "分离执行"), F("joinable"), F("get_id"), F("native_handle"), F("swap") };
        m["unique_ptr"] = { F("get"), F("reset"), F("release"), F("swap") };
        m["shared_ptr"] = { F("get"), F("reset"), F("swap"), F("use_count"), F("unique") };
        return m;
    }();
    return db;
}
static std::vector<CI> GenericMembers() {
    auto F = [](const char* l, const char* h = "") { return CI{ l, "函数", h, true }; };
    return { F("size"), F("length"), F("empty"), F("begin"), F("end"), F("clear"), F("swap"), F("at") };
}
// #include 头文件表
static const std::vector<std::pair<std::string, const char*>>& HeaderList() {
    static const std::vector<std::pair<std::string, const char*>> h = {
        {"iostream","输入输出流"}, {"cstdio","printf/scanf"}, {"cstdlib","atoi/rand"},
        {"cstring","strlen/strcpy"}, {"cmath","sin/sqrt/pow"}, {"cctype","isalpha/tolower"},
        {"ctime","time/clock"}, {"cassert","assert"}, {"climits","INT_MAX 等"},
        {"cfloat","浮点极限"}, {"string","std::string"}, {"string_view","轻量字符串视图"},
        {"vector","动态数组"}, {"array","定长数组"}, {"deque","双端队列"}, {"list","双向链表"},
        {"forward_list","单向链表"}, {"map","有序映射"}, {"multimap","多重映射"},
        {"set","有序集合"}, {"multiset","多重集合"}, {"unordered_map","哈希映射"},
        {"unordered_set","哈希集合"}, {"stack","栈"}, {"queue","队列(<queue>)"},
        {"algorithm","sort/find/reverse..."}, {"numeric","accumulate/iota"},
        {"functional","函数对象/bind"}, {"memory","智能指针"}, {"utility","pair/move"},
        {"tuple","元组"}, {"type_traits","类型萃取"}, {"iterator","迭代器辅助"},
        {"stdexcept","标准异常"}, {"exception","异常基类"}, {"random","随机数"},
        {"chrono","时间"}, {"ratio","比例"}, {"thread","线程"}, {"mutex","互斥量"},
        {"atomic","原子操作"}, {"condition_variable","条件变量"}, {"future","异步"},
        {"filesystem","文件系统(C++17)"}, {"regex","正则"}, {"sstream","字符串流"},
        {"fstream","文件流"}, {"iomanip","IO 格式控制"}, {"bitset","位集"},
        {"complex","复数"}, {"valarray","数值数组"}, {"optional","可选值(C++17)"},
        {"variant","多类型(C++17)"}, {"any","任意类型(C++17)"}, {"limits","数值极限"} };
    return h;
}
// std:: 命名空间常用项
static const std::vector<CI>& StdNamespaceItems() {
    static const std::vector<CI> v = {
        CI{"cout","对象","标准输出"}, CI{"cin","对象","标准输入"}, CI{"cerr","对象","标准错误"},
        CI{"endl","对象","换行+刷新"}, CI{"string","类型","动态字符串"}, CI{"wstring","类型","宽字符串"},
        CI{"vector","类型","动态数组"}, CI{"map","类型","有序映射"}, CI{"set","类型","有序集合"},
        CI{"unordered_map","类型","哈希映射"}, CI{"unordered_set","类型","哈希集合"},
        CI{"pair","类型","二元组"}, CI{"tuple","类型","元组"}, CI{"deque","类型","双端队列"},
        CI{"list","类型","链表"}, CI{"stack","类型","栈"}, CI{"queue","类型","队列"},
        CI{"thread","类型","线程"}, CI{"function","类型","可调用包装"}, CI{"optional","类型","可选值"},
        CI{"sort","函数","(首,末[,比较器])"}, CI{"find","函数"}, CI{"find_if","函数"},
        CI{"min","函数"}, CI{"max","函数"}, CI{"abs","函数"}, CI{"sqrt","函数"}, CI{"pow","函数"},
        CI{"reverse","函数"}, CI{"copy","函数"}, CI{"transform","函数"}, CI{"accumulate","函数(<numeric>)"},
        CI{"count","函数"}, CI{"count_if","函数"}, CI{"binary_search","函数"},
        CI{"lower_bound","函数"}, CI{"upper_bound","函数"},
        CI{"make_pair","函数"}, CI{"make_shared","函数"}, CI{"make_unique","函数(C++14)"},
        CI{"move","函数"}, CI{"forward","函数"}, CI{"swap","函数"},
        CI{"stoi","函数","字符串->int"}, CI{"stod","函数","字符串->double"}, CI{"to_string","函数"},
        CI{"getline","函数"} };
    return v;
}

// ===================================================================
//  标识符收集: 标识符 + 使用频率 + 函数名判定 (后跟 "(")
// ===================================================================
void CodeEditor::CollectIdents() {
    m_idents.clear();
    m_identFreq.clear();
    m_funcIdents.clear();
    const char* b = m_text.data();
    size_t i = 0, n = m_text.size();
    std::string lastWord;
    size_t lastEnd = 0;
    while (i < n) {
        if (IsIdentStart(b[i])) {
            size_t s = i;
            while (i < n && IsIdentChar(b[i])) i++;
            if (i - s >= 2) {
                std::string w = m_text.substr(s, i - s);
                m_idents.insert(w);
                m_identFreq[w]++;
                lastWord = w;
                lastEnd = i;      // 标识符结束位置 (下一个字符的下标)
            } else lastWord.clear();
        } else {
            if (!lastWord.empty()) {
                bool adjacent = (i == lastEnd) || ((b[i] == ' ' || b[i] == '\t') && i > lastEnd);
                if (adjacent && b[i] == '(') m_funcIdents.insert(lastWord);
                if (b[i] != ' ' && b[i] != '\t') lastWord.clear();
            }
            i++;
        }
    }
    m_identsDirty = false;
}

// ===================================================================
//  参数签名提示 (Calltip) —— 输入 "(" 弹出, 随 "," 高亮下一个参数
// ===================================================================

// 内置自由函数签名 (C 常用库 + 数学)
static const std::unordered_map<std::string, std::string>& SigDB() {
    static const std::unordered_map<std::string, std::string> s = {
        {"printf","int printf(const char* fmt, ...)"},
        {"scanf","int scanf(const char* fmt, ...)"},
        {"puts","int puts(const char* s)"},
        {"getchar","int getchar()"},
        {"strlen","size_t strlen(const char* s)"},
        {"strcpy","char* strcpy(char* dst, const char* src)"},
        {"strcmp","int strcmp(const char* a, const char* b)"},
        {"strcat","char* strcat(char* dst, const char* src)"},
        {"memcpy","void* memcpy(void* dst, const void* src, size_t n)"},
        {"memset","void* memset(void* p, int v, size_t n)"},
        {"malloc","void* malloc(size_t n)"},
        {"calloc","void* calloc(size_t n, size_t sz)"},
        {"free","void free(void* p)"},
        {"sqrt","double sqrt(double x)"},
        {"pow","double pow(double x, double y)"},
        {"abs","int abs(int x)"},
        {"fabs","double fabs(double x)"},
        {"sin","double sin(double x)"},
        {"cos","double cos(double x)"},
        {"tan","double tan(double x)"},
        {"atan2","double atan2(double y, double x)"},
        {"log","double log(double x)"},
        {"log10","double log10(double x)"},
        {"exp","double exp(double x)"},
        {"floor","double floor(double x)"},
        {"ceil","double ceil(double x)"},
        {"round","double round(double x)"},
        {"fmod","double fmod(double x, double y)"},
        {"rand","int rand()"},
        {"srand","void srand(unsigned int seed)"},
        {"time","time_t time(time_t* t)"},
        {"clock","clock_t clock()"},
        {"atoi","int atoi(const char* s)"},
        {"atof","double atof(const char* s)"},
        {"exit","void exit(int code)"},
        {"qsort","void qsort(void* base, size_t n, size_t sz, cmp)"},
    };
    return s;
}

bool CodeEditor::FindSignature(const std::string& name, const std::string& objType, std::string& sig) {
    // 0) 用户代码语义模型: 用户定义的函数/成员函数 (含真实签名)
    {
        EnsureCodeModel();
        std::string rsig, rret;
        if (m_code.FindFunctionSig(name, objType, rsig, rret)) {
            sig = TrimSig(rsig, 96);
            return true;
        }
    }
    // 0b) 头文件语义模型: 本文件 #include 的头文件里解析出的函数/成员函数
    if (CompilerAvail()) {
        EnsureHeaderModel();
        std::string rsig, rret;
        if (m_hdr.FindFunctionSig(name, objType, rsig, rret)) {
            sig = TrimSig(rsig, 96);
            return true;
        }
    }
    // 1) 成员访问: 符号索引方法签名 -> 内置成员表兜底
    if (!objType.empty()) {
        std::vector<DocRef> docs;
        for (auto& ed : g_app.editors) docs.push_back({ ed->m_uid, &ed->m_text });
        for (auto& s : SymbolIndex::I().MembersOfType(objType, docs))
            if (s.name == name && !s.sig.empty()) { sig = TrimSig(s.sig, 96); return true; }
        auto it = MemberDB().find(objType);
        if (it != MemberDB().end())
            for (auto& mi : it->second)
                if (mi.label == name && !mi.hint.empty()) { sig = name + "(...)" + mi.hint; return true; }
        return false;
    }
    // 2) 内置自由函数
    auto& db = SigDB();
    auto f = db.find(name);
    if (f != db.end()) { sig = f->second; return true; }
    // 3) 当前文档自由函数定义: "name(...)" 平衡到 ")", 且 ")" 后是 "{" 或 ";"
    size_t pos = 0;
    while ((pos = m_text.find(name, pos)) != std::string::npos) {
        size_t e = pos + name.size();
        bool lb = (pos == 0) || !IsIdentChar(m_text[pos - 1]);
        bool rb = (e >= m_text.size()) || !IsIdentChar(m_text[e]);
        if (lb && rb) {
            size_t k = e;
            while (k < m_text.size() && (m_text[k] == ' ' || m_text[k] == '\t')) k++;
            if (k < m_text.size() && m_text[k] == '(') {
                int depth = 0; size_t end = std::string::npos;
                for (size_t p = k; p < m_text.size(); p++) {
                    char c = m_text[p];
                    if (c == '(') depth++;
                    else if (c == ')') { depth--; if (depth == 0) { end = p + 1; break; } }
                    else if (c == '\n' && depth == 0) break;   // 参数列表一般不跨行起始
                }
                if (end != std::string::npos) {
                    size_t after = end;
                    while (after < m_text.size() &&
                           (m_text[after] == ' ' || m_text[after] == '\t' ||
                            m_text[after] == '\n' || m_text[after] == '\r')) after++;
                    if (after < m_text.size() && (m_text[after] == '{' || m_text[after] == ';')) {
                        std::string raw = m_text.substr(k, end - k);
                        for (char& c : raw) if (c == '\n' || c == '\r') c = ' ';
                        sig = name + TrimSig(raw, 96);
                        return true;
                    }
                }
            }
        }
        pos = e;
    }
    // 4) std:: 常用函数 (hint 即括号说明, 如 "(首,末[,比较器])")
    for (auto& si : StdNamespaceItems())
        if (si.label == name && !si.hint.empty() && si.hint[0] == '(') {
            sig = name + TrimSig(si.hint, 96);
            return true;
        }
    return false;
}

bool CodeEditor::TryOpenCalltip() {
    if (!g_app.settings.codeCompletion) { CloseCalltip(); return false; }
    if (m_cursor == 0 || m_cursor > m_text.size() || m_text[m_cursor - 1] != '(') return false;
    size_t paren = m_cursor - 1;
    // 光标前读最后一段标识符 (跳过空白)
    size_t e = paren;
    while (e > 0 && (m_text[e - 1] == ' ' || m_text[e - 1] == '\t')) e--;
    size_t s = e;
    while (s > 0 && IsIdentChar(m_text[s - 1])) s--;
    std::string name = m_text.substr(s, e - s);
    if (name.empty() || isdigit((unsigned char)name[0])) return false;
    // 成员访问前缀: obj. / obj->  -> 解析对象类型 (支持链式访问, 与成员补全一致)
    std::string objType;
    size_t q = s;
    while (q > 0 && (m_text[q - 1] == ' ' || m_text[q - 1] == '\t')) q--;
    size_t dot = std::string::npos;
    if (q > 0 && m_text[q - 1] == '.') dot = q - 1;
    else if (q >= 2 && m_text[q - 1] == '>' && m_text[q - 2] == '-') dot = q - 2;
    if (dot != std::string::npos) {
        size_t p = dot;
        int dp = 0, db = 0;
        while (p > 0) {
            size_t r = PrevChar(p);
            char c = m_text[r];
            if (c == ')') { dp++; p = r; continue; }
            if (c == ']') { db++; p = r; continue; }
            if (c == '(') { if (dp > 0) { dp--; p = r; continue; } break; }
            if (c == '[') { if (db > 0) { db--; p = r; continue; } break; }
            if (dp == 0 && db == 0) {
                if (IsIdentChar(c) || c == '.' || c == ':' || c == '>' || c == '-') { p = r; continue; }
                break;
            }
            p = r;
        }
        EnsureCodeModel();
        objType = m_code.EvalExprType(m_text.substr(p, dot - p), m_cursor);
    }
    std::string sig;
    if (!FindSignature(name, objType, sig)) return false;
    m_calltipOpen = true;
    m_calltipSig = sig;
    m_calltipParen = paren;
    m_calltipArg = 0;
    return true;
}

int CodeEditor::CountTopCommas(size_t a, size_t b) const {
    int depth = 0, cnt = 0;
    for (size_t i = a; i < b && i < m_text.size(); i++) {
        char c = m_text[i];
        if (c == '(' || c == '[' || c == '{') depth++;
        else if (c == ')' || c == ']' || c == '}') { if (depth > 0) depth--; }
        else if (c == ',' && depth == 0) cnt++;
    }
    return cnt;
}

// 光标是否在字符串字面量或注释里 (行级启发式)
bool CodeEditor::CursorInStringOrComment() const {
    if (m_text.empty() || m_cursor == 0) return false;
    size_t line = LineOf(m_cursor);
    if (line < m_lineInComment.size() && m_lineInComment[line]) return true;   // /* */ 内
    size_t ls = LineStart(line), le = LineEnd(line);
    bool inStr = false;
    for (size_t k = ls; k < m_cursor && k < le; k++) {
        char ch = m_text[k];
        if (!inStr && ch == '/' && k + 1 < le && m_text[k + 1] == '/') return true;  // 行注释
        if (inStr && ch == '\\') { k++; continue; }                                   // 转义
        if (ch == '"') inStr = !inStr;
    }
    return inStr;
}

// ---- 补全弹窗绘制辅助 (小熊猫风格: 匹配高亮 + 类型图标/配色) ----

// 类型配色 (与小熊猫按 StatementKind 着色对应)
static ImU32 CompKindColor(const char* kind) {
    if (!strcmp(kind, "关键字"))     return IM_COL32(255,170, 90,255);
    if (!strcmp(kind, "命名空间"))   return IM_COL32(150,190,240,255);
    if (!strcmp(kind, "类型"))       return IM_COL32(120,220,190,255);
    if (!strcmp(kind, "函数"))       return IM_COL32(235,205,120,255);
    if (!strcmp(kind, "变量"))       return IM_COL32(200,200,215,255);
    if (!strcmp(kind, "参数"))       return IM_COL32(190,205,225,255);
    if (!strcmp(kind, "枚举值"))     return IM_COL32(200,175,235,255);
    if (!strcmp(kind, "成员"))       return IM_COL32(150,210,140,255);
    if (!strcmp(kind, "对象"))       return IM_COL32(170,190,240,255);
    if (!strcmp(kind, "文件夹"))     return IM_COL32(235,190,110,255);
    if (!strcmp(kind, "本地头文件")) return IM_COL32(150,220,150,255);
    if (!strcmp(kind, "系统头文件")) return IM_COL32(150,190,230,255);
    if (!strcmp(kind, "头文件"))     return IM_COL32(150,190,230,255);
    return IM_COL32(180,180,190,255);
}

// 匹配位置: 优先整段前缀匹配, 否则退化为子序列匹配 (与小熊猫的 matched 前缀加粗一致)
static void CompMatchPositions(const std::string& label, const std::string& prefix, std::vector<int>& pos) {
    pos.clear();
    if (prefix.empty() || label.empty()) return;
    if (label.size() >= prefix.size()) {
        bool ok = true;
        for (size_t i = 0; i < prefix.size(); i++)
            if (std::tolower((unsigned char)label[i]) != std::tolower((unsigned char)prefix[i])) { ok = false; break; }
        if (ok) { for (size_t i = 0; i < prefix.size(); i++) pos.push_back((int)i); return; }
    }
    size_t j = 0;
    for (size_t i = 0; i < label.size() && j < prefix.size(); i++)
        if (std::tolower((unsigned char)label[i]) == std::tolower((unsigned char)prefix[j])) { pos.push_back((int)i); j++; }
    if (j != prefix.size()) pos.clear();
}

// 按匹配位置分段着色绘制文本 (匹配段用高亮色)
static void DrawCompLabel(ImDrawList* dl, ImVec2 p, const std::string& label,
                          const std::vector<int>& pos, ImU32 normal, ImU32 match) {
    ImFont* f = ImGui::GetFont();
    const float fs = ImGui::GetFontSize();
    if (pos.empty()) { dl->AddText(f, fs, p, normal, label.c_str()); return; }
    std::vector<char> ism(label.size(), 0);
    for (int k : pos) if (k >= 0 && k < (int)label.size()) ism[(size_t)k] = 1;
    size_t i = 0;
    while (i < label.size()) {
        bool m = ism[i] != 0;
        size_t j = i;
        while (j < label.size() && (ism[j] != 0) == m) j++;
        const char* s = label.c_str() + i;
        const char* e = label.c_str() + j;
        // 匹配段先画一层浅底, 再画高亮字, 视觉更接近小熊猫的加粗
        if (m) {
            float w = f->CalcTextSizeA(fs, FLT_MAX, 0, s, e).x;
            dl->AddRectFilled(ImVec2(p.x - 1, p.y - 1), ImVec2(p.x + w + 1, p.y + fs + 1),
                              IM_COL32(120,170,255,60), 2.0f);
        }
        dl->AddText(f, fs, p, m ? match : normal, s, e);
        p.x += f->CalcTextSizeA(fs, FLT_MAX, 0, s, e).x;
        i = j;
    }
}

void CodeEditor::SetCompletionSources(CompItem& item,const std::string& src,size_t pos,
                                     const CodeModel* model,const CSym* symbol) const {
    if(!g_app.plugins.HasCompletionProvider()) return;
    std::vector<CSym> sites;
    if(model && symbol) model->DeclarationSites(*symbol,sites);
    if(sites.empty()) { CSym site; site.declPos=pos; sites.push_back(site); }
    for(const auto& site:sites) {
        CompSource source; source.line=1;
        if(&src==&m_hdrUnit.text) m_hdrUnit.Locate(site.declPos,source.file,source.line);
        else {
            source.file=m_path;
            for(const auto& editor:g_app.editors) if(&editor->m_text==&src) { source.file=editor->m_path; break; }
            source.line=1+(int)std::count(src.begin(),src.begin()+std::min(site.declPos,src.size()),'\n');
        }
        source.doc=g_app.plugins.CompletionDocumentation(src,site.declPos);
        bool duplicate=false;
        for(const auto& known:item.sources) if(known.file==source.file && known.line==source.line) { duplicate=true; break; }
        if(!duplicate) item.sources.push_back(std::move(source));
    }
    if(!item.sources.empty()) {
        item.sourceIndex=0;
        for(size_t i=0;i<item.sources.size();++i) if(!item.sources[i].doc.empty()) { item.sourceIndex=(int)i; break; }
        item.doc=item.sources[item.sourceIndex].doc;
    }
}

void CodeEditor::GatherCandidates(const std::string& prefix, std::vector<CompItem>& out) {
    m_languagePending=false;
    if(language::Complete(*this)){out.clear();return;}
    if (!g_app.settings.codeCompletion) { out.clear(); return; }
    out.clear();
    g_app.plugins.BeginCompletion();

    // ---- #include 文件补全: 光标位于未闭合的 include 路径内时优先 ----
    // (走真实文件系统: 本地目录 + include 搜索路径 + 目录下钻, 而非内置头文件名表)
    {
        size_t incStart = 0; bool incQuote = false;
        if (InIncludeCompletion(incStart, incQuote)) {
            std::string phrase = (m_cursor > incStart) ? m_text.substr(incStart, m_cursor - incStart) : std::string();
            GatherHeaderCandidates(phrase, incQuote, out);
            return;
        }
    }

    // ---- 上下文检测: 光标前字符决定模式 ----
    enum Ctx { Word, Member, Ns } ctx = Word;
    std::string objType, nsName;
    size_t cs = m_compStart;
    char c1 = (cs >= 1) ? m_text[cs - 1] : '\0';
    if (cs >= 2 && c1 == ':' && m_text[cs - 2] == ':') {
        ctx = Ns;
        size_t p = cs - 2;
        // 支持 A::B:: 形式: 逐段向前收集 "ident (:: ident)*"
        while (true) {
            size_t q = p;
            while (q > 0 && IsIdentChar(m_text[PrevChar(q)])) q = PrevChar(q);
            if (q == p) break;
            p = q;
            if (p >= 2 && m_text[p - 1] == ':' && m_text[p - 2] == ':') p -= 2;
            else break;
        }
        nsName = m_text.substr(p, (cs - 2) - p);
        if (nsName.empty()) return;
    } else if (c1 == '.' || (cs >= 2 && c1 == '>' && m_text[cs - 2] == '-')) {
        ctx = Member;
        size_t dot = (c1 == '.') ? cs - 1 : cs - 2;
        // 向前提取对象表达式: 支持链式访问 (a.b->c / this->x / f(x).y / v[i].z)
        size_t p = dot;
        int dp = 0, db = 0;
        while (p > 0) {
            size_t q = PrevChar(p);
            char c = m_text[q];
            if (c == ')') { dp++; p = q; continue; }
            if (c == ']') { db++; p = q; continue; }
            if (c == '(') { if (dp > 0) { dp--; p = q; continue; } break; }
            if (c == '[') { if (db > 0) { db--; p = q; continue; } break; }
            if (dp == 0 && db == 0) {
                if (IsIdentChar(c) || c == '.' || c == ':' || c == '>' || c == '-') { p = q; continue; }
                break;
            }
            p = q;
        }
        std::string obj = m_text.substr(p, dot - p);
        EnsureCodeModel();
        objType = m_code.EvalExprType(obj, m_cursor);
        if ((objType.empty() || CodeModel::NormType(objType) == "auto") && CompilerAvail()) {
            // Header-defined types may be unknown to the document-only parser.
            // Query the expanded unit at the matching document position so local
            // scope still applies even when includes shift all byte offsets.
            EnsureHeaderModel();
            const int line = (int)LineOf(m_cursor) + 1;
            const size_t column = m_cursor - LineStart(line - 1);
            for (const auto& segment : m_hdrUnit.segments) {
                bool document = segment.fileId < 0 ? m_path.empty() :
                    segment.fileId < (int)m_hdrUnit.files.size() &&
                    m_hdrUnit.files[segment.fileId] == m_path;
                if (document && segment.line == line) {
                    objType = m_hdr.EvalExprType(obj, segment.outStart + std::min(column, segment.outLen));
                    break;
                }
            }
        }
    }

    // ---- 成员模式 ----
    if (ctx == Member) {
        if (objType.empty()) return;                       // 对象无名 (如数字后的 .): 不弹
        struct Scored { CI it; int sc; };
        std::vector<Scored> list;
        std::unordered_set<std::string> added;
        // 0) 文档语义模型: 用户自定义类型的成员 (作用域感知, 含基类链)
        {
            EnsureCodeModel();
            std::vector<CMember> mem;
            if (m_code.CollectMembers(objType, mem)) {
                for (auto& mm : mem) {
                    int base = ScoreOf(mm.name, prefix);
                    if (base < 0) continue;
                    if (!added.insert(mm.name).second) continue;
                    bool isFunc = (mm.kind == CSymKind::Function);
                    CI ci{ mm.name, isFunc ? "函数" : "成员",
                           isFunc ? mm.sig : mm.type, isFunc };
                    SetCompletionSources(ci,m_text,mm.declPos);
                    list.push_back({ std::move(ci), base + SymUsageBonus(mm.name) });
                }
            }
        }
        // 1) 头文件语义模型: 从本文件 #include 的头文件里解析出的类型成员
        {
            EnsureHeaderModel();
            std::vector<CMember> mem;
            if (m_hdr.CollectMembers(objType, mem)) {
                for (auto& mm : mem) {
                    int base = ScoreOf(mm.name, prefix);
                    if (base < 0) continue;
                    if (!added.insert(mm.name).second) continue;
                    bool isFunc = (mm.kind == CSymKind::Function);
                    CI ci{ mm.name, isFunc ? "函数" : "成员",
                           isFunc ? mm.sig : mm.type, isFunc };
                    SetCompletionSources(ci,m_hdrUnit.text,mm.declPos);
                    list.push_back({ std::move(ci), base + SymUsageBonus(mm.name) });
                }
            }
        }
        // 2) 未配置编译器 (无法解析头文件) 时, 回退内置表
        if (!CompilerAvail()) {
            // 符号索引: 解析打开文档 + 对应系统头文件
            {
                std::vector<DocRef> docs;
                for (auto& ed : g_app.editors) docs.push_back({ ed->m_uid, &ed->m_text });
                auto syms = SymbolIndex::I().MembersOfType(objType, docs);
                for (auto& s : syms) {
                    int base = ScoreOf(s.name, prefix);
                    if (base < 0) continue;
                    if (!added.insert(s.name).second) continue;
                    list.push_back({ CI{ s.name, s.isFunc ? "函数" : "成员",
                                         s.isFunc ? TrimSig(s.sig) : "", s.isFunc },
                                     base + SymUsageBonus(s.name) });
                }
            }
            // 内置成员表
            {
                auto it = MemberDB().find(objType);
                const std::vector<CI>* raw = nullptr;
                std::vector<CI> generic;
                if (it != MemberDB().end()) raw = &it->second;
                else if (list.empty()) { generic = GenericMembers(); raw = &generic; }
                if (raw) {
                    for (auto& mi : *raw) {
                        if (!added.insert(mi.label).second) continue;
                        int base = ScoreOf(mi.label, prefix);
                        if (base < 0) continue;
                        list.push_back({ mi, base + SymUsageBonus(mi.label) });
                    }
                }
            }
        }
        std::stable_sort(list.begin(), list.end(), [](const Scored& a, const Scored& b) { return a.sc > b.sc; });
        for (auto& m : list) out.push_back(std::move(m.it));
        return;
    }
    // ---- 命名空间模式: std/用户命名空间 ----
    if (ctx == Ns) {
        struct Scored { CI it; int sc; };
        std::vector<Scored> list;
        std::unordered_set<std::string> added;
        auto offerSym = [&](const CSym& s, int boost, const std::string* src) {
            if (s.kind != CSymKind::Type && s.kind != CSymKind::Enum && s.kind != CSymKind::Function &&
                s.kind != CSymKind::Variable && s.kind != CSymKind::Enumerator && s.kind != CSymKind::Namespace) return;
            int base = ScoreOf(s.name, prefix);
            if (base < 0) return;
            if (!added.insert(s.name).second) return;
            bool fn = (s.kind == CSymKind::Function);
            const char* kind = fn ? "函数"
                            : s.kind == CSymKind::Namespace ? "命名空间"
                            : (s.kind == CSymKind::Variable || s.kind == CSymKind::Enumerator) ? "变量" : "类型";
            std::string hint;
            if (fn) hint = s.sig;
            else if (s.kind == CSymKind::Variable || s.kind == CSymKind::Enumerator) hint = s.type;  // 变量显示类型
            CI ci{ s.name, kind, hint, fn };
            if (src) SetCompletionSources(ci,*src,s.declPos,src==&m_hdrUnit.text ? &m_hdr : &m_code,&s);
            list.push_back({ std::move(ci), base + boost + SymUsageBonus(s.name) });
        };
        EnsureCodeModel();
        std::vector<CSym> tmp;
        if (nsName == "std" || nsName == "boost") {
            m_code.NamespaceSymbols(nsName, true, tmp);
            for (auto& s : tmp) offerSym(s, 30, &m_text);
            if (CompilerAvail()) {
                EnsureHeaderModel();                     // 头文件符号 (std 里多为宏造成无 ns, 全量列出)
                for (auto& s : m_hdr.AllSymbols())
                    if (s.kind != CSymKind::Namespace || s.ns == nsName) offerSym(s, 0, &m_hdrUnit.text);
            } else {
                for (auto& si : StdNamespaceItems()) {   // 无编译器: 内置表兜底
                    int base = ScoreOf(si.label, prefix);
                    if (base < 0) continue;
                    list.push_back({ si, base + SymUsageBonus(si.label) });
                }
            }
        } else {                                         // 用户命名空间
            m_code.NamespaceSymbols(nsName, true, tmp);
            for (auto& s : tmp) offerSym(s, 20, &m_text);
            if (CompilerAvail()) {
                EnsureHeaderModel();
                m_hdr.NamespaceSymbols(nsName, true, tmp);
                for (auto& s : tmp) offerSym(s, 0, &m_hdrUnit.text);
            }
        }
        // nsName 也可能是"类型"而非命名空间 (std::ios:: / std::string:: / 用户类 Foo::):
        // 此时应列出该类型的成员 (含继承链, 如 ios -> basic_ios -> ios_base 的 in/out/goodbit)。
        if (list.empty()) {
            auto offerMember = [&](const CMember& mm, int boost, const std::string& src) {
                int base = ScoreOf(mm.name, prefix);
                if (base < 0) return;
                if (!added.insert(mm.name).second) return;
                bool fn = (mm.kind == CSymKind::Function);
                const char* kind = fn ? "函数"
                                : (mm.kind == CSymKind::Enumerator) ? "枚举值" : "成员";
                CI ci{ mm.name, kind, fn ? mm.sig : mm.type, fn };
                SetCompletionSources(ci,src,mm.declPos);
                list.push_back({ std::move(ci), base + boost + SymUsageBonus(mm.name) });
            };
            std::vector<CMember> mem;
            if (m_code.CollectMembers(nsName, mem))
                for (auto& mm : mem) offerMember(mm, 15, m_text);
            if (CompilerAvail()) {
                EnsureHeaderModel();
                std::vector<CMember> mem2;
                if (m_hdr.CollectMembers(nsName, mem2))
                    for (auto& mm : mem2) offerMember(mm, 0, m_hdrUnit.text);
            }
        }
        std::stable_sort(list.begin(), list.end(), [](const Scored& a, const Scored& b) { return a.sc > b.sc; });
        for (auto& m : list) out.push_back(std::move(m.it));
        return;
    }

    // ---- 符号模式: 关键字/类型/常用名 + 文档语义模型可见符号 (作用域感知) + 其他标签全局符号 ----
    EnsureWordPool();
    std::string pLow = CompToLower(prefix);
    struct Hit { int score; std::string label; const char* kind; std::string hint; bool func; std::string doc; const std::string* src=nullptr; const CodeModel* model=nullptr; CSym symbol; };
    std::vector<Hit> hits;
    hits.reserve(160);
    std::unordered_set<std::string> labels;
    for (auto& e : m_wordPool) {
        int sc = ScorePool(e, prefix, pLow);
        if (sc < 0) continue;
        labels.insert(e.label);
        hits.push_back({ sc + e.base, e.label, e.kind, e.hint, e.func });
    }
    EnsureCodeModel();
    auto addSym = [&](const CSym& s, int boost, const std::string& src, const CodeModel& model) {
        if (s.name.empty()) return; // Exact matches remain valid candidates (type, docs, namespace).
        if (!labels.insert(s.name).second) return;
        int base = ScoreOf(s.name, prefix);
        if (base < 0) return;
        const char* kind = "变量";
        std::string hint;
        bool func = false;
        switch (s.kind) {
            case CSymKind::Function:   kind = "函数"; func = true; hint = s.sig.empty() ? s.type : s.sig; break;
            case CSymKind::Parameter:  kind = "参数"; hint = s.type; break;
            case CSymKind::Type:       kind = "类型"; break;
            case CSymKind::Namespace:  kind = "命名空间"; break;
            case CSymKind::Enum:       kind = "类型"; hint = "enum"; break;
            case CSymKind::Enumerator: kind = "枚举值"; hint = s.type; break;
            case CSymKind::Member:     kind = "成员"; hint = s.type; break;
            default:                   kind = "变量"; hint = s.type; break;
        }
        hits.push_back({ base + boost + SymUsageBonus(s.name) + (s.isLocal ? 25 : 0),
                         s.name, kind, hint, func, {}, &src, &model, s });
    };
    {
        std::vector<CSym> vis;
        m_code.VisibleSymbols(m_cursor, vis);
        for (auto& s : vis) addSym(s, 70, m_text, m_code);
    }
    for (auto& ed : g_app.editors) {           // 其他打开标签的全局符号 (跨文件)
        if (ed.get() == this) continue;
        ed->EnsureCodeModel();
        std::vector<CSym> visible;
        m_code.FilterVisibleSymbols(m_cursor,ed->m_code.Globals(),visible);
        for (auto& s : visible) addSym(s, 55, ed->m_text, ed->m_code);
    }
    if (CompilerAvail()) {                      // 本文件 #include 的头文件里解析出的符号
        EnsureHeaderModel();
        // The expanded translation unit retains namespaces and using directives.
        // Query at the corresponding document offset, never flatten AllSymbols().
        size_t headerCursor=m_hdrUnit.text.size();
        int line=(int)LineOf(m_cursor)+1;
        size_t column=m_cursor-LineStart(line-1);
        for(const auto& segment:m_hdrUnit.segments) {
            bool document=segment.fileId<0 ? m_path.empty() :
                segment.fileId<(int)m_hdrUnit.files.size() && m_hdrUnit.files[segment.fileId]==m_path;
            if(document && segment.line==line) { headerCursor=segment.outStart+std::min(column,segment.outLen); break; }
        }
        std::vector<CSym> visible;
        m_hdr.VisibleSymbols(headerCursor,visible);
        for (auto& s : visible) addSym(s, 45, m_hdrUnit.text, m_hdr);
    }
    if (hits.empty()) return;
    size_t take = std::min<size_t>(40, hits.size());
    // 只需前 40: partial_sort 避免对整个候选集做全排序
    std::partial_sort(hits.begin(), hits.begin() + take, hits.end(),
                      [](const Hit& a, const Hit& b) {
        if (a.score != b.score) return a.score > b.score;
        if (a.label.size() != b.label.size()) return a.label.size() < b.label.size();
        return a.label < b.label;
    });
    out.reserve(take);
    for (size_t i = 0; i < take; i++) {
        CI ci{ hits[i].label, hits[i].kind, hits[i].hint, hits[i].func };
        ci.doc = std::move(hits[i].doc);
        if(hits[i].src) SetCompletionSources(ci,*hits[i].src,hits[i].symbol.declPos,hits[i].model,&hits[i].symbol);
        out.push_back(std::move(ci));
    }
}

// ===================================================================
//  补全
// ===================================================================
void CodeEditor::OpenCompletion() {
    if (!g_app.settings.codeCompletion) { CloseCompletion(); return; }
    bool isInc = false, isQuote = false;
    m_compStart = CompPrefixStart(isInc, isQuote);   // include 上下文下前缀含路径符号
    m_compPrefix = m_text.substr(m_compStart, m_cursor - m_compStart);
    m_compItems.clear();
    m_wordPoolDirty = true;   // 新一次补全会话: 重建候选池, 反映最新文档内容
    GatherCandidates(m_compPrefix, m_compItems);
    m_compIdx = 0;
    m_compScroll = 0;
    m_compTouched = false;                     // 新弹窗: 尚未人工选定
    m_compOpen = !m_compItems.empty() || m_languagePending;
}

void CodeEditor::UpdateCompletion() {
    if (!g_app.settings.codeCompletion) { CloseCompletion(); return; }
    if (!m_compOpen) return;
    if (m_cursor < m_compStart || m_cursor > m_text.size()) { CloseCompletion(); return; }
    m_compPrefix = m_text.substr(m_compStart, m_cursor - m_compStart);
    // 词法模式空前缀会列出全部符号 -> 关闭; 成员/命名空间/include 模式的空前缀合法
    if (m_compPrefix.empty()) {
        char pc = (m_compStart >= 1 && m_compStart <= m_text.size()) ? m_text[m_compStart - 1] : '\0';
        char pp = (m_compStart >= 2) ? m_text[m_compStart - 2] : '\0';
        bool memberCtx = (pc == '.') || (pc == '>' && pp == '-') || (pc == ':' && pp == ':');
        size_t hs = 0; bool hq = false;
        bool includeCtx = InIncludeCompletion(hs, hq);
        if (!memberCtx && !includeCtx) { CloseCompletion(); return; }
    }
    m_compItems.clear();
    GatherCandidates(m_compPrefix, m_compItems);
    if (m_compIdx >= (int)m_compItems.size()) m_compIdx = 0;
    if (m_compItems.empty() && !m_languagePending) m_compOpen = false;
}

bool CodeEditor::AcceptCompletion() {
    if (!g_app.settings.codeCompletion) { CloseCompletion(); return false; }
    if (!m_compOpen || m_compItems.empty()) return false;
    CompItem it = m_compItems[std::max(0, m_compIdx)];
    if(it.languageRevision) {
        if(!language::Current(*this,it.languageRevision) || m_cursor!=it.languageOffset || it.replaceStart>it.replaceEnd || it.replaceEnd>m_text.size()) {CloseCompletion();return false;}
        if(m_compPrefix.empty() && !m_compTouched){CloseCompletion();return false;}
        if(!it.func && it.insertion==m_text.substr(it.replaceStart,it.replaceEnd-it.replaceStart)){CloseCompletion();return false;}
        bool parentheses=it.func && it.insertion.find('(')==std::string::npos && (it.replaceEnd==m_text.size()||m_text[it.replaceEnd]!='(');
        if(parentheses)it.insertion+="()";
        PushUndo(it.replaceStart,m_text.substr(it.replaceStart,it.replaceEnd-it.replaceStart),it.insertion);
        EraseRange(it.replaceStart,it.replaceEnd,false);
        InsertAt(it.replaceStart,it.insertion,false);
        m_cursor=it.replaceStart+it.insertion.size()-(parentheses?1:0);m_selAnchor=m_cursor;m_selActive=false;m_dirty=true;m_cursorMoved=true;CloseCompletion();return true;
    }

    // 空前缀 (刚敲完 . / :: / " ) 且用户没按过方向键选定 -> 不抢 Tab/Enter (include 除外)
    {
        size_t hs = 0; bool hq = false;
        if (m_compPrefix.empty() && !m_compTouched && !InIncludeCompletion(hs, hq)) {
            CloseCompletion();
            return false;
        }
    }
    // 列表中存在与前缀完全相同的候选 (如打完整的关键字 int) -> 放行给缩进/换行
    for (auto& c : m_compItems)
        if (!c.func && c.label == m_compPrefix) { CloseCompletion(); return false; }
    size_t a = m_compStart, b = m_cursor;
    {   // include 上下文: 只替换最后一段路径, 保留已输入的目录前缀 (如 sys/)
        size_t hs = 0; bool hq = false;
        if (InIncludeCompletion(hs, hq) && hs <= b) {
            a = hs;
            std::string phrase = m_text.substr(hs, b - hs);
            size_t sep = phrase.find_last_of("/\\");
            if (sep != std::string::npos) a = hs + sep + 1;
        }
    }
    if (b > a) EraseRange(a, b);
    m_cursor = a;
    m_selAnchor = a;
    char nx = (m_cursor < m_text.size()) ? m_text[m_cursor] : '\0';
    if (it.isDir) {
        // #include 目录项: 插入 "name/" 后继续列出子目录 (与 RedPanda 的下钻一致)
        InsertText(it.label);              // label 以 '/' 结尾
        m_cursorMoved = true;
        m_compTouched = false;
        OpenCompletion();
        return true;
    }
    if (!it.fullPath.empty()) BumpFileUsage(it.fullPath);   // 记录使用频率 (排序用)
    else BumpSymUsage(it.label);                            // 符号使用频率 (跨文件, 排序用)
    if (it.func && nx != '(') {
        InsertText(it.label + "()");
        m_cursor = PrevChar(m_cursor);         // 光标放进括号内
        m_selAnchor = m_cursor;
    } else {
        InsertText(it.label);                  // 后面已有 "(" 则不加括号
    }
    m_cursorMoved = true;
    CloseCompletion();
    return true;
}

void CodeEditor::CloseCompletion() { m_languagePending=false;m_compOpen = false; m_compItems.clear(); }

// ===================================================================
//  查找
// ===================================================================
static bool ICharEq(char a, char b) {
    return std::tolower((unsigned char)a) == std::tolower((unsigned char)b);
}

static size_t IFind(const std::string& hay, const std::string& needle, size_t from) {
    size_t n = hay.size(), m = needle.size();
    if (m == 0 || from > n) return std::string::npos;
    if (m > n) return std::string::npos;
    for (size_t i = from; i + m <= n; i++) {
        bool ok = true;
        for (size_t j = 0; j < m; j++) {
            if (!ICharEq(hay[i + j], needle[j])) { ok = false; break; }
        }
        if (ok) return i;
    }
    return std::string::npos;
}

void CodeEditor::RecomputeMatches() {
    m_findMatches.clear();
    m_findIdx = -1;
    if (m_findStr.empty()) return;
    size_t pos = 0;
    while (true) {
        size_t p;
        if (m_findCaseSensitive)
            p = m_text.find(m_findStr, pos);
        else
            p = IFind(m_text, m_findStr, pos);
        if (p == std::string::npos) break;
        if (m_findWholeWord) {
            size_t e = p + m_findStr.size();
            bool lOk = (p == 0) || !IsIdentChar(m_text[p - 1]);
            bool rOk = (e >= m_text.size()) || !IsIdentChar(m_text[e]);
            if (!(lOk && rOk)) { pos = p + 1; continue; }
        }
        m_findMatches.push_back(p);
        pos = p + m_findStr.size();
        if (pos >= m_text.size()) break;
    }
}

void CodeEditor::FindNext(bool back) {
    if (m_findStr.empty() || m_findMatches.empty()) return;
    int n = (int)m_findMatches.size();
    if (back) m_findIdx = (m_findIdx - 1 + n) % n;
    else      m_findIdx = (m_findIdx + 1) % n;
    size_t p = m_findMatches[m_findIdx];
    m_selAnchor = p;
    m_cursor = p + m_findStr.size();
    m_selActive = true;
    m_cursorMoved = true;
}

// ===================================================================
//  滚动可见
// ===================================================================
void CodeEditor::EnsureCursorVisible() {
    if (m_lines.empty()) return;
    float lineH = LineH();
    float gutter = GutterW();
    size_t line = LineOf(m_cursor);
    int row = VisibleRowOfLine((int)line);
    if (row < 0) row = (int)line;   // 折叠尚未被 Draw 展开时按逻辑行估算
    float cy = row * lineH;
    float viewW = m_drawSize.x - gutter;
    float viewH = m_drawSize.y;
    if (viewW <= 0 || viewH <= 0) return;

    if (cy < m_scrollY + 4) m_scrollY = cy - 4;
    if (cy + lineH > m_scrollY + viewH - 4) m_scrollY = cy + lineH - viewH + 4;

    float cx = CharX(m_cursor) + gutter;
    if (cx < m_scrollX + 16) m_scrollX = cx - 16;
    if (cx > m_scrollX + viewW - 16) m_scrollX = cx - viewW + 16;

    m_scrollY = std::max(0.0f, m_scrollY);
    m_scrollX = std::max(0.0f, m_scrollX);
    m_cursorMoved = false;
}

// ===================================================================
//  渲染辅助
// ===================================================================
static void DrawRange(ImDrawList* dl, ImFont* f, float fs, ImVec2 pos, float originX, const char* b, size_t start, size_t end, ImU32 col, float charW) {
    // 无 Tab 时整段绘制 (快路径)
    bool hasTab = false;
    for (size_t i = start; i < end; i++) if (b[i] == '\t') { hasTab = true; break; }
    if (!hasTab) {
        dl->AddText(f, fs, pos, col, b + start, b + end);
        return;
    }
    // 有 Tab: 逐段。Tab 停靠点以"行文本起点 originX"为第 0 列计算 (与 CharX 保持一致),
    // 而不是以本段起点为准, 否则同一行的各 token 会因基准不同而错位/重叠。
    float x = pos.x;
    size_t i = start;
    while (i < end) {
        if (b[i] == '\t') {
            size_t col = (size_t)((x - originX) / charW);
            x = originX + (float)((col / EditorTabWidth() + 1) * EditorTabWidth()) * charW;
            i++;
        } else {
            size_t e = i + 1;
            while (e < end && b[e] != '\t') e++;
            dl->AddText(f, fs, ImVec2(x, pos.y), col, b + i, b + e);
            x += f->CalcTextSizeA(fs, FLT_MAX, -1.0f, b + i, b + e, nullptr).x;
            i = e;
        }
    }
}

// ===================================================================
//  查找栏
// ===================================================================
void CodeEditor::DrawFindBar() {
    if (!m_findOpen) return;
    ImGui::SetNextItemWidth(220);
    if (ui::InputTextStd("##find", m_findStr,
                         ImGuiInputTextFlags_AutoSelectAll | ImGuiInputTextFlags_EnterReturnsTrue)) {
        RecomputeMatches();
        FindNext(false);
    }
    ImGui::SameLine();
    if (ImGui::Button("上一个")) FindNext(true);
    ImGui::SameLine();
    if (ImGui::Button("下一个")) FindNext(false);
    ImGui::SameLine();
    ImGui::TextDisabled("%d/%d", m_findIdx + 1, (int)m_findMatches.size());
    ImGui::SameLine();
    bool oldCs = m_findCaseSensitive, oldWw = m_findWholeWord;
    if (ImGui::Checkbox("Aa", &m_findCaseSensitive)) RecomputeMatches();
    ImGui::SameLine();
    if (ImGui::Checkbox("整词", &m_findWholeWord)) RecomputeMatches();
    ImGui::SameLine();
    if (ImGui::Button("关闭")) { m_findOpen = false; m_findMatches.clear(); m_findIdx = -1; }
    // ---- 替换行 ----
    ImGui::SetNextItemWidth(220);
    if (ui::InputTextStd("##replace", m_replaceStr, ImGuiInputTextFlags_EnterReturnsTrue))
        ReplaceOne();
    ImGui::SameLine();
    if (ImGui::Button("替换")) ReplaceOne();
    ImGui::SameLine();
    if (ImGui::Button("全部替换")) ReplaceAll();
    ImGui::SameLine();
    ImGui::TextDisabled("Ctrl+G 跳转行 | F2 书签 | Shift+F2 上书签");
}

// 替换当前选中的匹配 (无选中则先跳到下一个)
void CodeEditor::ReplaceOne() {
    if (m_findStr.empty()) return;
    RecomputeMatches();
    if (m_findMatches.empty()) return;
    if (m_findIdx < 0 || m_findIdx >= (int)m_findMatches.size()) { FindNext(false); return; }
    size_t pos = m_findMatches[m_findIdx];
    bool matchOk;
    if (m_findCaseSensitive)
        matchOk = (m_text.compare(pos, m_findStr.size(), m_findStr) == 0);
    else {
        matchOk = true;
        for (size_t j = 0; j < m_findStr.size() && pos + j < m_text.size(); j++)
            if (!ICharEq(m_text[pos + j], m_findStr[j])) { matchOk = false; break; }
    }
    if (!matchOk) { RecomputeMatches(); return; }
    EraseRange(pos, pos + m_findStr.size());
    InsertText(m_replaceStr);
    RecomputeMatches();
    m_findIdx = -1;
    FindNext(false);   // 跳到下一个匹配
}

// 全部替换 (支持撤销栈: 每次替换独立撤销, 多次 Ctrl+Z 回退)
void CodeEditor::ReplaceAll() {
    if (m_findStr.empty()) return;
    RecomputeMatches();
    int count = 0;
    // 从后往前替换, 保持前面的匹配偏移不变
    for (int i = (int)m_findMatches.size() - 1; i >= 0; i--) {
        size_t pos = m_findMatches[i];
        bool matchOk;
        if (m_findCaseSensitive)
            matchOk = (m_text.compare(pos, m_findStr.size(), m_findStr) == 0);
        else {
            matchOk = true;
            for (size_t j = 0; j < m_findStr.size() && pos + j < m_text.size(); j++)
                if (!ICharEq(m_text[pos + j], m_findStr[j])) { matchOk = false; break; }
        }
        if (!matchOk) continue;
        size_t origPos = m_cursor;
        size_t origAnchor = m_selAnchor;
        bool origSel = m_selActive;
        // 临时把光标放到 pos, 选区不激活, 让 EraseRange + InsertAt 使用 pos 作为操作位置
        m_cursor = pos + m_findStr.size();
        m_selAnchor = pos;
        m_selActive = true;
        EraseRange(pos, pos + m_findStr.size(), true);
        InsertAt(pos, m_replaceStr, true);
        m_cursor = origPos;
        m_selAnchor = origAnchor;
        m_selActive = origSel;
        count++;
    }
    if (count > 0) {
        InvalidateAll();
        RebuildLines();
        m_cursorMoved = true;
        g_app.MarkDiag();
        g_app.console.Append("[编辑] 全部替换: " + std::to_string(count) + " 处 (可 Ctrl+Z 撤销)\n",
                             IM_COL32(140, 200, 160, 255));
    }
    m_findMatches.clear();
    m_findIdx = -1;
}

// Ctrl+G 跳转行输入栏
void CodeEditor::DrawGotoBar() {
    if (!m_gotoOpen) return;
    if (m_gotoFocus) { ImGui::SetKeyboardFocusHere(); m_gotoFocus = false; }
    ImGui::SetNextItemWidth(120);
    if (ImGui::InputInt("##goto", &m_gotoLine, 0, 0, ImGuiInputTextFlags_EnterReturnsTrue)) {
        GotoLine(m_gotoLine);
        m_gotoOpen = false;
    }
    ImGui::SameLine();
    if (ImGui::Button("跳转")) { GotoLine(m_gotoLine); m_gotoOpen = false; }
    ImGui::SameLine();
    if (ImGui::Button("取消")) m_gotoOpen = false;
}

// ===================================================================
//  右键菜单
// ===================================================================
void CodeEditor::DrawContextMenu() {
    if (!ImGui::BeginPopup("##edctx")) return;
    bool hasSel = m_selActive && SelBegin() != SelEnd();
    int curLine = (int)LineOf(m_cursor);
    bool isBM = HasBookmark(curLine);

    if (ImGui::MenuItem("撤销", "Ctrl+Z")) Undo();
    if (ImGui::MenuItem("重做", "Ctrl+Y")) Redo();
    ImGui::Separator();
    if (ImGui::MenuItem("剪切", "Ctrl+X", false, hasSel)) {
        ImGui::SetClipboardText(GetSelectionText().c_str());
        DeleteSelection();
    }
    if (ImGui::MenuItem("复制", "Ctrl+C", false, hasSel)) {
        ImGui::SetClipboardText(GetSelectionText().c_str());
    }
    if (ImGui::MenuItem("粘贴", "Ctrl+V")) {
        const char* clip = ImGui::GetClipboardText();
        if (clip && *clip) {
            std::string s = clip;
            std::string t;
            t.reserve(s.size());
            for (char c : s) {
                if (c == '\r') continue;
                if (c == '\t') t += g_app.settings.insertSpaces?std::string(TabWidth(),' '):"\t";
                else t += c;
            }
            InsertText(t);
        }
    }
    ImGui::Separator();
    if (ImGui::MenuItem(isBM ? "取消书签" : "切换书签", "Ctrl+F2")) ToggleBookmark();
    if (ImGui::MenuItem("下一个书签", "F2", false, !m_bookmarks.empty())) NextBookmark();
    if (ImGui::MenuItem("上一个书签", "Shift+F2", false, !m_bookmarks.empty())) PrevBookmark();
    if (ImGui::MenuItem("清除全部书签", nullptr, false, !m_bookmarks.empty())) ClearAllBookmarks();
    ImGui::Separator();
    if (ImGui::MenuItem("跳转到定义", "Ctrl+B / Ctrl+点击", false, !IdentAt(m_cursor).empty())) GotoDefinition();
    if (ImGui::MenuItem("切换断点", "Ctrl+行号")) ToggleBreakpoint((int)LineOf(m_cursor)+1);
    ImGui::Separator();
    if (ImGui::MenuItem("全选", "Ctrl+A")) SelectAll();
    ImGui::EndPopup();
}

// ===================================================================
//  主绘制
// ===================================================================
void CodeEditor::Draw() {
    ImGuiWindow* editorHost=ImGui::GetCurrentWindow();
    RefreshLineChanges();
    if (!g_app.settings.codeCompletion) { CloseCompletion(); CloseCalltip(); }
    if (m_linesDirty) RebuildLines();
    if (m_commentDirty) RebuildCommentCache();
    ClampCursor();

    // 折叠: 重建可视行缓存; 光标若落入折叠区(撤销/跳转导致)则自动展开
    RebuildVisCache();
    if (!m_folds.empty()) {
        int cl = (int)LineOf(m_cursor);
        if (IsLineHidden(cl)) ExpandFoldContaining(cl);
    }
    RebuildVisCache();

    // Calltip 实时状态: 逗号序号随文本计算; 光标退到 "(" 之前则关闭
    if (m_calltipOpen) {
        if (m_cursor <= m_calltipParen || m_cursor > m_text.size() ||
            m_text[m_calltipParen] != '(')
            CloseCalltip();
        else
            m_calltipArg = CountTopCommas(m_calltipParen + 1, m_cursor);
    }

    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImFont* f = g_app.editorFont ? g_app.editorFont : ImGui::GetFont();
    float fs = f ? f->LegacySize : 16.0f;
    float lineH = LineH();
    float charW = CharW();
    float gutter = GutterW();

    // ---- 查找栏 / 跳转栏 ----
    DrawFindBar();
    DrawGotoBar();

    // ---- 编辑器区域 ----
    ImVec2 avail = ImGui::GetContentRegionAvail();
    m_drawPos = ImGui::GetCursorScreenPos();
    m_drawSize = avail;
    if (m_drawSize.x < 50 || m_drawSize.y < 50) return;   // 注意: 必须在 PushFont 之前提前返回

    ImGui::PushFont(f,fs);

    ImRect rect(m_drawPos, ImVec2(m_drawPos.x + m_drawSize.x, m_drawPos.y + m_drawSize.y));

    // 交互 (InvisibleButton)
    ImGui::InvisibleButton("##ed_main", m_drawSize, ImGuiButtonFlags_MouseButtonLeft);
    bool hovered = ImGui::IsItemHovered();
    bool active = ImGui::IsItemActive();
    m_mouseInEditor = hovered || active;

    // 点击定位 (行号栏 = 选整行)
    if (hovered && io.MouseClicked[0]) {
        ImGui::SetWindowFocus();
        m_wordSelecting = false;
        bool inGutter = io.MousePos.x < rect.Min.x + gutter;
        if(!inGutter && io.KeyAlt) {
            float x=io.MousePos.x-rect.Min.x-gutter+m_scrollX,y=io.MousePos.y-rect.Min.y+m_scrollY;
            if(io.KeyShift){m_columnSelecting=true;m_columnX=x;m_columnY=y;ColumnSelect(x,y,x,y);}
            else AddCaret(HitTest(x,y));
            m_mouseSelecting=false;
        } else {m_carets.clear();

        if (inGutter) {
            int row = (int)((io.MousePos.y - rect.Min.y + m_scrollY) / lineH);
            int line = LineOfVisualRow(row);
            bool foldToggled = false;
            // 行号栏左侧折叠标记区: 点击切换该行折叠
            if (io.KeyCtrl) { ToggleBreakpoint(line + 1); foldToggled = true; }
            else if (io.MousePos.x - rect.Min.x < 11.0f) {
                BuildFoldCandidates();
                bool isCand = std::binary_search(m_foldCands.begin(), m_foldCands.end(), line);
                bool isAnchor = false;
                for (auto& ff : m_folds) if (ff.first == line) { isAnchor = true; break; }
                if (isCand || isAnchor) { ToggleFoldAt(line); foldToggled = true; }
            }
            if (!foldToggled) {
                m_lineAnchor = io.KeyShift ? LineOf(m_selAnchor) : (size_t)line;
                m_selAnchor = LineStart(m_lineAnchor);
                // 行尾 = 下一行行首 (最后一行 = 文本尾)
                m_cursor = ((size_t)line + 1 < m_lines.size()) ? LineStart((size_t)line + 1) : m_text.size();
                m_selActive = true;
                m_lineSelecting = true;
                m_mouseSelecting = true;
            }
        } else {
            float cx = io.MousePos.x - (rect.Min.x + gutter) + m_scrollX;
            float cy = io.MousePos.y - rect.Min.y + m_scrollY;
            if (io.KeyCtrl) {
                // Ctrl+点击: 跳转定义 (不进入拖选)
                m_cursor = HitTest(cx, cy);
                m_cursorMoved = true;
                CloseCompletion();
                CloseCalltip();
                GotoDefinition();
            } else {
                m_cursor = HitTest(cx, cy);
                if (!io.KeyShift) m_selAnchor = m_cursor;
                m_selActive = io.KeyShift && m_cursor != m_selAnchor;
                m_lineSelecting = false;
                m_mouseSelecting = true;
                m_cursorMoved = true;
                CloseCompletion();
                CloseCalltip();
            }
        }
    }
        }
    // Drag outside the viewport to scroll continuously, without jumping the caret.
    if (m_mouseSelecting && active && io.MouseDown[0]) {
        auto speed = [&](float distance) { return std::min(lineH * 40, distance * 10) * io.DeltaTime; };
        if (io.MousePos.y < rect.Min.y) m_scrollY -= speed(rect.Min.y - io.MousePos.y);
        if (io.MousePos.y > rect.Max.y) m_scrollY += speed(io.MousePos.y - rect.Max.y);
        m_scrollY = std::max(0.0f, m_scrollY);
        if (!m_lineSelecting) {
            if (io.MousePos.x < rect.Min.x + gutter) m_scrollX -= speed(rect.Min.x + gutter - io.MousePos.x);
            if (io.MousePos.x > rect.Max.x - 12) m_scrollX += speed(io.MousePos.x - rect.Max.x + 12);
            m_scrollX = std::max(0.0f, m_scrollX);
        }
        float mouseY = std::max(rect.Min.y, std::min(rect.Max.y - 1, io.MousePos.y));
        if (m_lineSelecting) {
            int row = (int)((mouseY - rect.Min.y + m_scrollY) / lineH);
            size_t line = (size_t)LineOfVisualRow(row);
            if (line >= m_lineAnchor) {
                m_selAnchor = LineStart(m_lineAnchor);
                m_cursor = line + 1 < m_lines.size() ? LineStart(line + 1) : m_text.size();
            } else {
                m_selAnchor = m_lineAnchor + 1 < m_lines.size() ? LineStart(m_lineAnchor + 1) : m_text.size();
                m_cursor = LineStart(line);
            }
        } else {
            float cx = std::max(0.0f, std::min(rect.Max.x - 12, io.MousePos.x) - rect.Min.x - gutter) + m_scrollX;
            float cy = mouseY - rect.Min.y + m_scrollY;
            m_cursor = HitTest(cx, cy);
            if (m_wordSelecting) {
                size_t a=m_cursor, end=m_cursor;
                while(a>0 && IsIdentChar(m_text[PrevChar(a)])) a=PrevChar(a);
                while(end<m_text.size() && IsIdentChar(m_text[end])) end=NextChar(end);
                if(m_cursor<m_wordAnchorA) { m_selAnchor=m_wordAnchorB; m_cursor=a; }
                else { m_selAnchor=m_wordAnchorA; m_cursor=std::max(m_wordAnchorB,end); }
            }
        }
        m_selActive = (m_cursor != m_selAnchor);
        m_cursorMoved = false;
    }
    if(m_columnSelecting&&io.MouseDown[0])ColumnSelect(m_columnX,m_columnY,io.MousePos.x-rect.Min.x-gutter+m_scrollX,io.MousePos.y-rect.Min.y+m_scrollY);
    if(!io.MouseDown[0])m_columnSelecting=false;
    if (!io.MouseDown[0]) { m_mouseSelecting = false; m_lineSelecting = false; m_wordSelecting = false; }

    // 双击选词
    if (hovered && !io.KeyAlt && io.MouseDoubleClicked[0]) {
        float cx = io.MousePos.x - (rect.Min.x + gutter) + m_scrollX;
        float cy = io.MousePos.y - rect.Min.y + m_scrollY;
        size_t p = HitTest(cx, cy);
        size_t a = p, b = p;
        while (a > 0 && IsIdentChar(m_text[PrevChar(a)])) a = PrevChar(a);
        while (b < m_text.size() && IsIdentChar(m_text[b])) b = NextChar(b);
        if (a != b) {
            m_selAnchor = a;
            m_cursor = b;
            m_wordAnchorA=a; m_wordAnchorB=b; m_wordSelecting=true;
            m_selActive = true;
            m_cursorMoved = true;
        }
    }

    // 右键菜单
    if (hovered && io.MouseClicked[1]) {
        m_carets.clear();
        float cx = io.MousePos.x - (rect.Min.x + gutter) + m_scrollX;
        float cy = io.MousePos.y - rect.Min.y + m_scrollY;
        m_cursor = HitTest(cx, cy);
        m_selAnchor = m_cursor;
        m_selActive = false;
        m_cursorMoved = true;
        ImGui::OpenPopup("##edctx");
    }

    // 滚轮
    if (hovered) {
        if (io.MouseWheel != 0.0f) {
            if (io.KeyShift) m_scrollX -= io.MouseWheel * charW * 6.0f;
            else m_scrollY -= io.MouseWheel * std::clamp(g_app.settings.scrollLines,1,12) * lineH;
        }
        if (io.MouseWheelH != 0.0f) m_scrollX -= io.MouseWheelH * charW * 6.0f;
    }

    // ---- 键盘 ----
    // 自绘编辑器自己处理所有按键/字符; 仅在窗口内查找栏 InputText 聚焦时
    // (WantCaptureKeyboard=true) 让位给它, 避免同一队列的字符被消费两次。
    // NavEnableKeyboard 已关闭, 所以 WantCaptureKeyboard 只在 InputText 聚焦时为 true,
    // 正常编辑时光标/方向键/Enter/Tab/Backspace/字符/Ctrl+Space 全部由编辑器处理。
    bool windowFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    if (windowFocused && !io.WantCaptureKeyboard && !MultiKeyboard()) {
        // 补全弹窗导航 (编辑键在下方始终可用, 不再被弹窗分支独占)
        bool keyUsed = false;
        if (m_compOpen && !m_compItems.empty()) {
            if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
                m_compIdx = (m_compIdx - 1 + (int)m_compItems.size()) % (int)m_compItems.size();
                m_compTouched = true; keyUsed = true;
            } else if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
                m_compIdx = (m_compIdx + 1) % (int)m_compItems.size();
                m_compTouched = true; keyUsed = true;
            }
            // Ctrl+Tab 不触发补全/缩进, 留给全局标签切换
            if ((!io.KeyCtrl && (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter))) ||
                (!io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Tab))) {
                if (AcceptCompletion()) keyUsed = true;   // 未接受(空匹配等)则放行给缩进/换行
            }
            else if (ImGui::IsKeyPressed(ImGuiKey_Escape)) { CloseCompletion(); keyUsed = true; }
        }
        {
            if (io.KeyCtrl) {
                if (ImGui::IsKeyPressed(ImGuiKey_Z)) { Undo(); if (m_compOpen) CloseCompletion(); }
                if (ImGui::IsKeyPressed(ImGuiKey_Y)) { Redo(); if (m_compOpen) CloseCompletion(); }
                if (ImGui::IsKeyPressed(ImGuiKey_A)) SelectAll();
                if (ImGui::IsKeyPressed(ImGuiKey_C) && m_selActive && SelBegin() != SelEnd())
                    ImGui::SetClipboardText(GetSelectionText().c_str());
                if (ImGui::IsKeyPressed(ImGuiKey_X) && m_selActive && SelBegin() != SelEnd()) {
                    ImGui::SetClipboardText(GetSelectionText().c_str());
                    DeleteSelection();
                }
                if (ImGui::IsKeyPressed(ImGuiKey_V)) {
                    const char* clip = ImGui::GetClipboardText();
                    if (clip && *clip) {
                        std::string t;
                        for (char c : std::string(clip)) {
                            if (c == '\r') continue;
                            t += (c == '\t') ? (g_app.settings.insertSpaces?std::string(TabWidth(),' '):"\t") : std::string(1, c);
                        }
                        InsertText(t);
                    }
                }
                if (ImGui::IsKeyPressed(ImGuiKey_F)) { m_findOpen = true; RecomputeMatches(); }
                if (ImGui::IsKeyPressed(ImGuiKey_H)) { m_findOpen = true; RecomputeMatches(); }
                if (ImGui::IsKeyPressed(ImGuiKey_G)) { m_gotoOpen = true; m_gotoFocus = true; }
                if (ImGui::IsKeyPressed(ImGuiKey_Space)) OpenCompletion();
            }
            if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow))  MoveLeft(io.KeyShift, io.KeyCtrl);
            if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) MoveRight(io.KeyShift, io.KeyCtrl);
            if (ImGui::IsKeyPressed(ImGuiKey_UpArrow) && !keyUsed)    MoveUp(io.KeyShift);
            if (ImGui::IsKeyPressed(ImGuiKey_DownArrow) && !keyUsed)  MoveDown(io.KeyShift);
            if (ImGui::IsKeyPressed(ImGuiKey_Home))       MoveHome(io.KeyShift, io.KeyCtrl);
            if (ImGui::IsKeyPressed(ImGuiKey_End))        MoveEnd(io.KeyShift, io.KeyCtrl);
            if (ImGui::IsKeyPressed(ImGuiKey_PageUp))     PageUp(io.KeyShift);
            if (ImGui::IsKeyPressed(ImGuiKey_PageDown))   PageDown(io.KeyShift);
            // 光标移动离开当前词尾 -> 关闭补全弹窗
            if (!keyUsed && m_compOpen &&
                (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) || ImGui::IsKeyPressed(ImGuiKey_RightArrow) ||
                 ImGui::IsKeyPressed(ImGuiKey_Home)      || ImGui::IsKeyPressed(ImGuiKey_End)))
                CloseCompletion();
            if (ImGui::IsKeyPressed(ImGuiKey_Backspace)) {
                if (io.KeyCtrl) {           // Ctrl+Backspace: 按词删除
                    if (m_selActive) DeleteSelection();
                    else { size_t a = WordLeft(m_cursor); if (a < m_cursor) EraseRange(a, m_cursor); }
                } else DoBackspace();
                // 弹窗打开时退格 = 缩短前缀并刷新候选 (VS Code 行为)
                if (m_compOpen) {
                    size_t hs = 0; bool hq = false;
                    bool inc = InIncludeCompletion(hs, hq);
                    if (m_cursor < m_compStart || m_cursor > m_text.size()) CloseCompletion();
                    else if (m_cursor == m_compStart && !inc) CloseCompletion();   // 空前缀词法模式
                    else UpdateCompletion();                                       // include 空前缀仍列出当前目录
                }
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Delete)) {
                if (io.KeyCtrl) {           // Ctrl+Delete: 向后按词删除
                    if (m_selActive) DeleteSelection();
                    else { size_t b = WordRight(m_cursor); if (b > m_cursor) EraseRange(m_cursor, b); }
                } else DoDelete();
            }
            if (!keyUsed && !io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Tab)) DoTab();  // Ctrl+Tab 留给全局快捷键 (默认不缩进)
            if (!keyUsed && (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter))) DoEnter();
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_B)) GotoDefinition();
            if (ImGui::IsKeyPressed(ImGuiKey_F2)) {
                if (io.KeyCtrl) ToggleBookmark();   // Ctrl+F2 加/解书签
                else if (io.KeyShift) PrevBookmark(); // Shift+F2 上一个书签
                else NextBookmark();                // F2 下一个
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                if (m_findOpen) { m_findOpen = false; m_findMatches.clear(); m_findIdx = -1; }
                if (m_gotoOpen) m_gotoOpen = false;
                if (m_calltipOpen) CloseCalltip();
            }
        }

        // 字符输入 (含 IME)
        for (int n = 0; n < io.InputQueueCharacters.Size; n++) {
            unsigned int c = io.InputQueueCharacters[n];
            if (c == 0) continue;
            // 回车/换行由 Enter 键处理 (DoEnter 智能缩进); Tab 由 Tab 键处理
            // (DoTab / 补全接受), 忽略重复的字符事件
            if (c == '\r' || c == '\n' || c == '\t') continue;
            if (c < 128) {
                char ch = (char)c;
                if (IsIdentChar(ch)) {
                    // 输入标识符字符: 插入后自动更新/打开补全
                    DoAutoBracket(ch);          // 非括号字符 -> 直接插入
                    size_t hs = 0; bool hq = false;
                    if (InIncludeCompletion(hs, hq)) {
                        // #include 路径内: 继续文件补全 (在字符串中, 不能用 CursorInStringOrComment 关闭)
                        if (m_compOpen) UpdateCompletion(); else if(ShouldAutoComplete()) OpenCompletion();
                    } else if (CursorInStringOrComment()) CloseCompletion();
                    else if (!m_compOpen) {if(ShouldAutoComplete())OpenCompletion();}
                    else UpdateCompletion();
                } else {
                    // 输入 "}" 且行首到光标全为空白: 自动反缩进一层, 与 "{" 对齐
                    if (ch == '}') {
                        size_t ls = LineStart(LineOf(m_cursor));
                        bool onlyBlank = true;
                        for (size_t p = ls; p < m_cursor; p++)
                            if (m_text[p] != ' ' && m_text[p] != '\t') { onlyBlank = false; break; }
                        if(g_app.settings.autoIndent&&onlyBlank&&m_cursor>ls)DoBackspace();
                    }
                    DoAutoBracket(ch);          // 括号等 -> 配对插入
                    CloseCompletion();
                    // #include 文件补全: 输入 " < 或 / \ . - + 等路径字符后继续提示
                    size_t ihs = 0; bool ihq = false;
                    if (InIncludeCompletion(ihs, ihq)) {
                        if (m_compOpen) UpdateCompletion(); else if(ShouldAutoComplete()) OpenCompletion();
                        continue;   // 文件补全上下文不再走成员/签名提示
                    }
                    // 访问符触发成员/命名空间/头文件补全: .   ->   ::
                    bool accessor = false;
                    if (ch == '.')
                        accessor = (m_cursor >= 2 && !isdigit((unsigned char)m_text[m_cursor - 2])); // 排除 3.14
                    else if (ch == '>')
                        accessor = (m_cursor >= 2 && m_text[m_cursor - 2] == '-');
                    else if (ch == ':')
                        accessor = (m_cursor >= 2 && m_text[m_cursor - 2] == ':');
                    if (accessor && g_app.settings.autoCompletion && !CursorInStringOrComment()) {
                        m_compStart = m_cursor;    // 空前缀, 列出全部成员
                        m_compPrefix.clear();
                        GatherCandidates(m_compPrefix, m_compItems);
                        m_compIdx = 0;
                        m_compTouched = false;
                        m_compOpen = !m_compItems.empty() || m_languagePending;
                    }
                    // 参数签名提示: "(" 弹出, ")" 关闭
                    if (ch == '(') {
                        if (!CursorInStringOrComment()) TryOpenCalltip();
                    } else if (ch == ')' && m_calltipOpen) {
                        CloseCalltip();
                    }
                }
            } else {
                // 中文等非 ASCII: 正常插入, 关闭英文/C++ 补全
                InsertText(utils::WToU8(std::wstring(1, (wchar_t)c)));
                CloseCompletion();
            }
        }
        io.InputQueueCharacters.resize(0);
    }

    // 键盘移动后: 若光标落入折叠区(上/下方向越过), 自动展开该折叠
    if (!m_folds.empty()) {
        int cl2 = (int)LineOf(m_cursor);
        if (IsLineHidden(cl2)) ExpandFoldContaining(cl2);
    }

    // 确保光标可见
    if (m_cursorMoved) EnsureCursorVisible();

    // ---- 滚动范围 (按可视行计算, 折叠后的内容高度) ----
    RebuildVisCache();
    size_t visRows = m_visLine.size();
    float contentH = (float)visRows * lineH;
    float maxScrollY = std::max(0.0f, contentH - m_drawSize.y);
    m_scrollY = std::max(0.0f, std::min(m_scrollY, maxScrollY));

    // Measure pixels, including tabs/UTF-8; invalidate when text or font changes.
    if (m_maxLineDirty || m_widthFont != f || m_widthFontSize != fs || m_widthTabWidth!=TabWidth()) {
        m_maxLineWidth = 0;
        for (size_t li=0; li<m_lines.size(); ++li) m_maxLineWidth=std::max(m_maxLineWidth,CharX(LineEnd(li)));
        m_maxLineDirty=false; m_widthFont=f; m_widthFontSize=fs; m_widthTabWidth=TabWidth();
    }
    float contentW = m_maxLineWidth + 80.0f;
    float maxScrollX = std::max(0.0f, contentW - (m_drawSize.x - gutter));
    m_scrollX = std::max(0.0f, std::min(m_scrollX, maxScrollX));

    EnsureCodeModel();
    RebuildDecorations();
    dl->PushClipRect(rect.Min, rect.Max, true);
    // ---- 绘制背景 ----
    dl->AddRectFilled(rect.Min, rect.Max, g_app.colors.editorBg);

    int firstRow = (int)(m_scrollY / lineH);
    int lastRow = (int)((m_scrollY + m_drawSize.y) / lineH) + 1;
    if (firstRow < 0) firstRow = 0;
    if (lastRow > (int)visRows) lastRow = (int)visRows;

    size_t cursorLine = LineOf(m_cursor);
    int cursorRow = VisibleRowOfLine((int)cursorLine);   // 光标所在可视行 (保护逻辑保证可见)
    size_t selA = SelBegin(), selB = SelEnd();
    bool hasSel = m_selActive && selA != selB;
    const std::string currentWord = (hasSel||!g_app.settings.highlightWordOccurrences) ? std::string() : IdentAt(m_cursor);
    const char* b = m_text.data();

    // 当前行高亮
    float curY = rect.Min.y + cursorRow * lineH - m_scrollY;
    if (g_app.settings.highlightCurrentLine&&cursorRow >= firstRow && cursorRow < lastRow) {
        dl->AddRectFilled(ImVec2(rect.Min.x, curY), ImVec2(rect.Max.x, curY + lineH), g_app.colors.curLineBg);
    }

    // 每行绘制 (仅遍历可视行; li 为该可视行对应的逻辑行)
    size_t di = 0;   // 诊断双指针 (m_diags 按 line 升序)
    // 查找高亮指针: m_findMatches 按偏移升序且与逻辑行同向推进, 使整屏高亮 O(匹配数), 而非每行全表扫描
    size_t findScan = 0, findLen = m_findStr.size();
    for (int ri = firstRow; ri < lastRow; ri++) {
        int li = m_visLine[ri];
        float y = rect.Min.y + ri * lineH - m_scrollY;
        if (y + lineH < rect.Min.y || y > rect.Max.y) continue;

        // 行号栏背景
        dl->AddRectFilled(ImVec2(rect.Min.x, y), ImVec2(rect.Min.x + gutter, y + lineH), g_app.colors.gutterBg);

        if (g_app.settings.highlightCurrentLine&&(size_t)li == cursorLine)
            dl->AddRectFilled(ImVec2(rect.Min.x,y), ImVec2(rect.Min.x+gutter,y+lineH), g_app.EditorColorValue(EditorColor::CurrentGutter));

        // 诊断行: 淡色行背景 + 行号栏标记条 (错误红 / 警告黄)
        if(g_app.debugView.active && g_app.debugView.stopped && g_app.debugView.line==li+1 &&
           _wcsicmp(utils::U8ToW(g_app.debugView.file).c_str(),m_path.c_str())==0)
            dl->AddRectFilled(ImVec2(rect.Min.x+gutter,y),ImVec2(rect.Max.x,y+lineH),g_app.EditorColorValue(EditorColor::ActiveBreakpoint));
        bool original = (size_t)li < m_originalLines.size() && m_originalLines[li];
        if(g_app.settings.showChangeMarkers)dl->AddRectFilled(ImVec2(rect.Min.x+gutter-4,y+1), ImVec2(rect.Min.x+gutter-1,y+lineH-1),
            g_app.EditorColorValue(original ? EditorColor::OriginalLine : EditorColor::AddedLine));
        if(std::binary_search(m_breakpoints.begin(),m_breakpoints.end(),li+1))
            dl->AddCircleFilled(ImVec2(rect.Min.x+16,y+lineH/2),3.5f,g_app.EditorColorValue(EditorColor::Breakpoint));
        while (di < m_diags.size() && m_diags[di].line - 1 < li) di++;
        const Diag* dg = nullptr;
        if (di < m_diags.size() && m_diags[di].line - 1 == li) {
            dg = &m_diags[di];
            for (size_t k = di; k < m_diags.size() && m_diags[k].line - 1 == li; k++)
                if (m_diags[k].isErr) { dg = &m_diags[k]; break; }   // 同行多条取错误优先
        }
        if (dg) {
            ImU32 bg = dg->isErr ? g_app.colors.diagErrBg : g_app.colors.diagWarnBg;
            ImU32 mk = dg->isErr ? g_app.colors.diagErrMark : g_app.colors.diagWarnMark;
            dl->AddRectFilled(ImVec2(rect.Min.x + gutter, y), ImVec2(rect.Max.x, y + lineH), bg);
            dl->AddRectFilled(ImVec2(rect.Min.x + 3, y + 3), ImVec2(rect.Min.x + 7, y + lineH - 3), mk);
        }
        // 书签标记 (行号左侧橙色小方块)
        if (std::binary_search(m_bookmarks.begin(), m_bookmarks.end(), (int)li))
            dl->AddRectFilled(ImVec2(rect.Min.x + 3, y + lineH * 0.25f),
                              ImVec2(rect.Min.x + 7, y + lineH * 0.75f),
                              IM_COL32(255, 170, 40, 255));

        // 行号
        char num[16];
        snprintf(num, sizeof(num), "%d", g_app.settings.relativeLineNumbers&&li!=int(cursorLine)?std::abs(li-int(cursorLine)):li+1);
        float numW = ImGui::CalcTextSize(num).x;

        // 折叠标记 (+ 展开 / - 收起), 位于行号左侧
        {
            bool folded = false, isCand = false;
            for (auto& ff : m_folds) if (ff.first == li) { folded = true; break; }
            if (!folded) {
                BuildFoldCandidates();
                isCand = std::binary_search(m_foldCands.begin(), m_foldCands.end(), li);
            }
            if (folded || isCand) {
                ImU32 c = g_app.EditorColorValue(EditorColor::Fold);
                const char* m = folded ? "+" : "-";
                dl->AddText(ImVec2(rect.Min.x + 3, y + (lineH - fs) * 0.5f), c, m);
            }
        }
        if(g_app.settings.showLineNumbers)dl->AddText(ImVec2(rect.Min.x + gutter - numW - 8.0f, y + (lineH - fs) * 0.5f),
                    g_app.colors.lineNum, num);

        // 文本区
        size_t ls = LineStart((size_t)li), le = LineEnd((size_t)li);
        ImFontBaked* baked = f->GetFontBaked(fs);
        float textTop = y + (lineH - (baked->Ascent + baked->Descent)) * 0.5f;

        const float textLeft=rect.Min.x+gutter, textRight=rect.Max.x-12;
        dl->PushClipRect(ImVec2(textLeft,rect.Min.y),ImVec2(textRight,rect.Max.y),true);
        // Indentation layers are independent from syntax and selection colors.
        int indent=(size_t)li<m_indentColumns.size() ? m_indentColumns[li] : 0;
        for (int column=0, level=0; column+TabWidth()<=indent; column+=TabWidth(),++level) {
            float ix=textLeft + column*charW-m_scrollX;
            ImU32 rainbow=g_app.EditorColorValue((EditorColor)((int)EditorColor::Bracket1+level%4));
            if(g_app.settings.rainbowIndentFill) {
                ImVec4 fill=ImGui::ColorConvertU32ToFloat4(rainbow); fill.w *= 0.08f;
                dl->AddRectFilled(ImVec2(ix,y),ImVec2(ix+TabWidth()*charW,y+lineH),ImGui::ColorConvertFloat4ToU32(fill));
            }
            if(g_app.settings.showIndentGuides || g_app.settings.rainbowIndentLines)
                dl->AddLine(ImVec2(ix+0.5f,y),ImVec2(ix+0.5f,y+lineH),g_app.settings.rainbowIndentLines ? rainbow : g_app.EditorColorValue(EditorColor::Indent));
        }
        if(!currentWord.empty()) for(const auto& token:m_tokCache[li]) {
            if(token.type!=TokenKind::Default && token.type!=TokenKind::TypeName) continue;
            size_t pos=ls+token.start;
            if(token.len==currentWord.size() && m_text.compare(pos,token.len,currentWord)==0) {
                float left=textLeft+CharX(pos)-m_scrollX, right=textLeft+CharX(pos+token.len)-m_scrollX;
                dl->AddRectFilled(ImVec2(left,y),ImVec2(right,y+lineH),g_app.EditorColorValue(EditorColor::WordHighlight));
            }
        }
        if (hasSel && selB>ls && selA<=le) {
            size_t a=std::max(ls,selA), bb=std::min(le,selB);
            float x0=textLeft+CharX(a)-m_scrollX;
            float x1=selB>le && le<m_text.size() ? textRight : textLeft+CharX(bb)-m_scrollX;
            if(x1>x0) dl->AddRectFilled(ImVec2(x0,y),ImVec2(x1,y+lineH),g_app.colors.selection);
        }
        for(const auto& caret:m_carets){
            size_t a=std::min(caret.anchor,caret.cursor),end=std::max(caret.anchor,caret.cursor);
            if(end>ls && a<le){size_t begin=std::max(a,ls),finish=std::min(end,le);if(begin<finish)dl->AddRectFilled(ImVec2(textLeft+CharX(begin)-m_scrollX,y),ImVec2(textLeft+CharX(finish)-m_scrollX,y+lineH),g_app.colors.selection);}
        }
        // 查找高亮
        if (findScan < m_findMatches.size()) {
            const auto& fm = m_findMatches;
            // 跳过已整体结束在本行之前的匹配 (偏移升序 + 逐行递增, 均摊 O(1))
            while (findScan < fm.size() && fm[findScan] + findLen <= ls) findScan++;
            for (size_t k = findScan; k < fm.size() && fm[k] < le; k++) {
                size_t m = fm[k];
                float x0 = rect.Min.x + gutter + CharX(std::max(ls, m)) - m_scrollX;
                float x1 = rect.Min.x + gutter + CharX(std::min(le, m + findLen)) - m_scrollX;
                dl->AddRectFilled(ImVec2(x0, y), ImVec2(x1, y + lineH), g_app.colors.findMatch);
            }
        }

        // 文本
        if (le > ls) {
            float xBase = rect.Min.x + gutter - m_scrollX;
            ImVec2 tp(xBase, textTop);
            {
                // 每行分词缓存 (编辑时失效), 避免每帧重新分词
                EnsureTokens((size_t)li);
                // 顺序推进 x 绘制 (每字符只测量一次, 替代每 token 从行首重算的 O(n^2))
                float x = xBase;
                size_t done = ls;
                auto widthOf = [&](size_t a, size_t e2) {
                    float w = 0;
                    size_t i = a;
                    while (i < e2) {
                        if (b[i] == '\t') {
                            // Tab 停靠点以行文本起点 xBase 为第 0 列 (与 CharX / DrawRange 一致)
                            float nx = xBase + (float)((((size_t)((x + w - xBase) / charW)) / EditorTabWidth() + 1) * EditorTabWidth()) * charW;
                            w += nx - (x + w);
                            i++;
                        } else {
                            size_t e3 = NextChar(i);
                            w += f->CalcTextSizeA(fs, FLT_MAX, -1.0f, b + i, b + e3, nullptr).x;
                            i = e3;
                        }
                    }
                    return w;
                };
                for (auto& t : m_tokCache[(size_t)li]) {
                    if (t.start > done - ls) x += widthOf(done, ls + t.start);
                    size_t te = ls + t.start + t.len;
                    ImU32 col = SyntaxColor(t,ls,le);
                    DrawRange(dl, f, fs, ImVec2(x,textTop),xBase,b,ls+t.start,te,col,charW);
                    // Escapes have their own style while the literal remains a single lexer token.
                    if(g_app.settings.syntaxHighlight && t.type==TokenKind::String) for(size_t p=ls+t.start; p+1<te; ++p) if(b[p]=='\\') {
                        size_t e=p+2;
                        if(b[p+1]=='x') while(e<te && std::isxdigit((unsigned char)b[e])) ++e;
                        else if(b[p+1]=='u'||b[p+1]=='U') e=std::min(te,p+(b[p+1]=='u'?6u:10u));
                        else if(b[p+1]>='0'&&b[p+1]<='7') while(e<te && e<p+4 && b[e]>='0'&&b[e]<='7') ++e;
                        DrawRange(dl,f,fs,ImVec2(x+widthOf(ls+t.start,p),textTop),xBase,b,p,e,g_app.EditorColorValue(EditorColor::Escape),charW);
                        p=e-1;
                    }
                    if(t.type==TokenKind::Default && !IsIdentStart(b[ls+t.start])) {
                        float offset=0;
                        for(size_t p=ls+t.start;p<te;++p) {
                            float w=b[p]=='\t' ? xBase+(float)((((size_t)((x+offset-xBase)/charW))/4+1)*4)*charW-(x+offset) : f->CalcTextSizeA(fs,FLT_MAX,-1,b+p,b+p+1).x;
                            if(p<m_bracketLevel.size() && m_bracketLevel[p]!=-1) {
                                if(g_app.settings.syntaxHighlight && (m_bracketLevel[p]==-2 || g_app.settings.rainbowBrackets)) {
                                    ImU32 color=g_app.EditorColorValue(m_bracketLevel[p]==-2 ? EditorColor::Illegal : (EditorColor)((int)EditorColor::Bracket1+m_bracketLevel[p]));
                                    DrawRange(dl,f,fs,ImVec2(x+offset,textTop),xBase,b,p,p+1,color,charW);
                                }
                            } else if(g_app.settings.showWhitespace && (b[p]==' '||b[p]=='\t')) {
                                ImU32 mark=g_app.EditorColorValue(EditorColor::Whitespace);
                                if(b[p]==' ') dl->AddCircleFilled(ImVec2(x+offset+w/2,textTop+fs*0.55f),1,mark);
                                else { dl->AddLine(ImVec2(x+offset+2,textTop+fs/2),ImVec2(x+offset+w-2,textTop+fs/2),mark); }
                            }
                            offset+=w;
                        }
                    }
                    if(hasSel) {
                        size_t a=std::max(ls+t.start,selA), end=std::min(te,selB);
                        if(end>a) {
                            float left=textLeft+CharX(a)-m_scrollX, right=textLeft+CharX(end)-m_scrollX;
                            dl->PushClipRect(ImVec2(left,y),ImVec2(right,y+lineH),true);
                            DrawRange(dl,f,fs,ImVec2(x,textTop),xBase,b,ls+t.start,te,g_app.EditorColorValue(EditorColor::SelectedText),charW);
                            dl->PopClipRect();
                        }
                    }
                    x += widthOf(ls + t.start, te);
                    done = te;
                }
            }
        }
        dl->PopClipRect();
    }

    // Additional carets and rectangular selections share the editor clip.
    dl->PushClipRect(ImVec2(rect.Min.x+gutter,rect.Min.y),rect.Max,true);
    for(const auto& caret:m_carets) {
        int row=VisibleRowOfLine(int(LineOf(caret.cursor)));if(row>=0){float x=rect.Min.x+gutter+CharX(caret.cursor)-m_scrollX,y=rect.Min.y+row*lineH-m_scrollY;dl->AddRectFilled(ImVec2(x,y+2),ImVec2(x+2,y+lineH-2),g_app.colors.caret);}
    }
    dl->PopClipRect();
    // 光标
    if (windowFocused) {
        float blink = (float)fmod(ImGui::GetTime() * 2.2, 1.0);
        bool on = !g_app.settings.caretBlink||blink < 0.6f;
        if (on) {
            float cx = rect.Min.x + gutter + CharX(m_cursor) - m_scrollX;
            float cy = rect.Min.y + cursorRow * lineH - m_scrollY;
            // 光标宽度随字号缩放 (至少 2px), 高度占行高 (上下留边)
            float caretW = std::max(2.0f, std::round(fs * 0.10f));
            float caretY = cy + 2.0f;
            float caretH = lineH - 4.0f;
            dl->AddRectFilled(ImVec2(cx, caretY), ImVec2(cx + caretW, caretY + caretH), g_app.colors.caret);
        }
    }

    // 括号匹配高亮 (光标两侧为括号时, 高亮它与匹配括号)
    {
        size_t bp = (size_t)-1;
        if (m_cursor < m_text.size() && strchr("([{)]}", m_text[m_cursor])) bp = m_cursor;
        else if (m_cursor > 0 && strchr("([{)]}", m_text[m_cursor - 1])) bp = m_cursor - 1;
        if (bp != (size_t)-1) {
            int mp = FindMatchingBracket(bp);
            if (mp >= 0) {
                ImU32 bc = g_app.colors.bracketBg;
                for (int p : { (int)bp, mp }) {
                    size_t ln = LineOf((size_t)p);
                    int r = VisibleRowOfLine((int)ln);
                    if (r < 0) continue;   // 匹配括号在折叠区内
                    float x = rect.Min.x + gutter + CharX((size_t)p) - m_scrollX;
                    float y = rect.Min.y + r * lineH - m_scrollY;
                    float w = CharX((size_t)p + 1) - CharX((size_t)p);
                    if (w < 2.0f) w = charW;
                    dl->AddRectFilled(ImVec2(x, y + 1), ImVec2(x + w, y + lineH - 1), bc);
                }
            }
        }
    }

    // 诊断悬停提示
    if (!m_diags.empty() && m_mouseInEditor &&
        io.MousePos.x >= rect.Min.x + gutter && io.MousePos.x <= rect.Max.x) {
        int hovRow = (int)((io.MousePos.y - rect.Min.y + m_scrollY) / lineH);
        int hovLine = LineOfVisualRow(hovRow);
        for (auto& d : m_diags) {
            if (d.line - 1 > hovLine) break;
            if (d.line - 1 == hovLine) {
                ImGui::SetTooltip("%s", d.msg.c_str());
                break;
            }
        }
    }

    // ---- 滚动条 ----
    const float barW = 10.0f;
    if (maxScrollY > 0.0f) {
        float trackH = m_drawSize.y - (maxScrollX > 0 ? barW : 0);
        float thumbH = std::max(20.0f, trackH * (m_drawSize.y / contentH));
        float thumbY = rect.Min.y + (trackH - thumbH) * (m_scrollY / maxScrollY);
        ImRect track(rect.Max.x - barW - 2, rect.Min.y, rect.Max.x - 2, rect.Min.y + trackH);
        dl->AddRectFilled(track.Min, track.Max, g_app.colors.scrollTrack);
        dl->AddRectFilled(ImVec2(track.Min.x, thumbY), ImVec2(track.Max.x, thumbY + thumbH), g_app.colors.scrollThumb);
        ImGui::SetCursorScreenPos(track.Min);
        ImGui::InvisibleButton("##vbar", track.GetSize());
        if (ImGui::IsItemActive() && trackH > 0) {
            float ny = io.MousePos.y - track.Min.y - thumbH * 0.5f;
            m_scrollY = (ny / (trackH - thumbH)) * maxScrollY;
            m_scrollY = std::max(0.0f, std::min(m_scrollY, maxScrollY));
        }
    }
    if (maxScrollX > 0.0f) {
        float trackW = m_drawSize.x - gutter - (maxScrollY > 0 ? barW : 0);
        float thumbW = std::max(20.0f, trackW * ((m_drawSize.x - gutter) / contentW));
        float thumbX = rect.Min.x + gutter + (trackW - thumbW) * (m_scrollX / maxScrollX);
        ImRect track(rect.Min.x + gutter, rect.Max.y - barW - 2, rect.Min.x + gutter + trackW, rect.Max.y - 2);
        dl->AddRectFilled(track.Min, track.Max, g_app.colors.scrollTrack);
        dl->AddRectFilled(ImVec2(thumbX, track.Min.y), ImVec2(thumbX + thumbW, track.Max.y), g_app.colors.scrollThumb);
        ImGui::SetCursorScreenPos(track.Min);
        ImGui::InvisibleButton("##hbar", track.GetSize());
        if (ImGui::IsItemActive() && trackW > 0) {
            float nx = io.MousePos.x - track.Min.x - thumbW * 0.5f;
            m_scrollX = (nx / (trackW - thumbW)) * maxScrollX;
            m_scrollX = std::max(0.0f, std::min(m_scrollX, maxScrollX));
        }
    }

    // 鼠标形状: 代码文本区用 I-beam (行号栏 / 滚动条 / 编辑器外保持默认箭头)
    if (m_mouseInEditor) {
        ImVec2 mp = io.MousePos;
        bool onText = mp.x >= rect.Min.x + gutter && mp.x <= rect.Max.x - barW &&
                      mp.y >= rect.Min.y && mp.y <= rect.Max.y;
        if (onText) ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);
    }

    dl->PopClipRect();
    // 右键菜单
    DrawContextMenu();

    // ---- 补全弹窗 (小熊猫风格: 匹配高亮 / 类型图标 / 选中行) ----
    if (m_compOpen && !m_compItems.empty()) {
        float cx = rect.Min.x + gutter + CharX(m_cursor) - m_scrollX;
        float cy = rect.Min.y + cursorRow * lineH - m_scrollY + lineH + 2;
        const int total = (int)m_compItems.size();
        const ImGuiViewport* vp = ImGui::GetWindowViewport();
        const float rowH = ImGui::GetTextLineHeight() + 6.0f;
        const float maxH = std::max(1.0f, vp->WorkSize.y - 12.0f);
        const bool hasHint=m_compIdx>=0 && m_compIdx<total && !m_compItems[m_compIdx].hint.empty();
        const bool hasDocumentation=g_app.plugins.HasCompletionProvider() && !m_compItems[std::clamp(m_compIdx,0,total-1)].languageRevision;
        const float detailsH = hasDocumentation ? std::min(260.0f, maxH * 0.44f) : hasHint ? std::min(84.0f,maxH*0.18f) : 0.0f;
        const int visible = std::max(1, std::min(std::clamp(g_app.settings.completionRows,4,16), (int)((maxH - detailsH - 85.0f) / rowH)));
        if (m_compIdx < 0) m_compIdx = 0;
        if (m_compIdx >= total) m_compIdx = total - 1;
        if (m_compIdx < m_compScroll) m_compScroll = m_compIdx;
        if (m_compIdx >= m_compScroll + visible) m_compScroll = m_compIdx - visible + 1;
        if (m_compScroll > std::max(0, total - visible)) m_compScroll = std::max(0, total - visible);
        if (m_compScroll < 0) m_compScroll = 0;
        const int shown = std::min(visible, total - m_compScroll);

        // 高亮前缀: 文件补全只按路径最后一段匹配
        std::string hlPrefix = m_compPrefix;
        size_t hsep = hlPrefix.find_last_of("/\\");
        if (hsep != std::string::npos) hlPrefix = hlPrefix.substr(hsep + 1);

        float wKind = 0, wText = 0;
        for (int k = 0; k < shown; k++) {
            const CompItem& it = m_compItems[m_compScroll + k];
            wKind = std::max(wKind, ImGui::CalcTextSize(it.kind).x);
            float w = ImGui::CalcTextSize(it.label.c_str()).x;
            if (!it.hint.empty()) w += ImGui::CalcTextSize(TrimSig(it.hint).c_str()).x + 10.0f;
            wText = std::max(wText, w);
        }
        const float winW = std::min(std::max(340.0f, 20.0f + wKind + 8.0f + wText + 38.0f),
                                    std::min(float(std::clamp(g_app.settings.completionWidth,360,1000)), std::max(1.0f, vp->WorkSize.x - 12.0f)));
        const float rowW = std::max(1.0f, winW - 12.0f);
        const float winH = std::min(maxH, shown * rowH + detailsH + 85.0f);
        const float minX = vp->WorkPos.x + 6.0f, minY = vp->WorkPos.y + 6.0f;
        cx = std::max(minX, std::min(cx, vp->WorkPos.x + vp->WorkSize.x - winW - 6.0f));
        if (cy + winH > vp->WorkPos.y + vp->WorkSize.y - 6.0f)
            cy = cy - lineH - 4.0f - winH;
        cy = std::max(minY, std::min(cy, vp->WorkPos.y + vp->WorkSize.y - winH - 6.0f));
        ImGui::SetNextWindowPos(ImVec2(cx, cy));
        ImGui::SetNextWindowSize(ImVec2(winW, winH));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6, 5));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::GetStyleColorVec4(ImGuiCol_WindowBg));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.32f, 0.55f, 0.85f, 0.85f));
        if (ImGui::Begin("##completion", nullptr,
                         ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoFocusOnAppearing |
                         ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoCollapse)) {
            const ImU32 textCol = g_app.colors.text;
            const ImU32 matchCol = IM_COL32(255,214,120,255);
            const ImU32 hintCol  = IM_COL32(150,150,160,255);
            ImGui::PushStyleColor(ImGuiCol_Text, textCol);
            ImGui::Text("%s", m_compPrefix.empty() ? "补全" : m_compPrefix.c_str());
            ImGui::PopStyleColor();
            ImGui::SameLine();
            ImGui::TextDisabled("%d/%d", m_compIdx + 1, total);
            ImGui::Separator();

            ImDrawList* pdl = ImGui::GetWindowDrawList();
            ImFont* font = ImGui::GetFont();
            const float fs = ImGui::GetFontSize();
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 0));
            for (int k = 0; k < shown; k++) {
                int i = m_compScroll + k;
                const CompItem& it = m_compItems[i];
                bool sel = (i == m_compIdx);
                ImU32 kc = CompKindColor(it.kind);
                ImVec2 rp = ImGui::GetCursorScreenPos();
                ImGui::PushID(i);
                bool clicked = ImGui::InvisibleButton("##row", ImVec2(rowW, rowH));
                bool hov = ImGui::IsItemHovered();
                if (hov) { m_compIdx = i; m_compTouched = true; }   // 悬停选定
                ImGui::PopID();
                if (sel)
                    pdl->AddRectFilled(rp, ImVec2(rp.x + rowW, rp.y + rowH), IM_COL32(60,90,140,190), 3.0f);
                else if (hov)
                    pdl->AddRectFilled(rp, ImVec2(rp.x + rowW, rp.y + rowH), IM_COL32(60,70,90,120), 3.0f);
                if (sel)   // 左侧类型色条
                    pdl->AddRectFilled(rp, ImVec2(rp.x + 3.0f, rp.y + rowH), kc, 1.5f);
                float ty = rp.y + (rowH - fs) * 0.5f;
                // 类型图标: 圆点 + 类型名 (按类型着色)
                pdl->AddCircleFilled(ImVec2(rp.x + 12.0f, rp.y + rowH * 0.5f), 3.5f, kc, 12);
                pdl->AddText(font, fs, ImVec2(rp.x + 20.0f, ty), kc, it.kind);
                // 候选文本 + 匹配高亮
                std::vector<int> mpos;
                CompMatchPositions(it.label, hlPrefix, mpos);
                ImVec2 lp(rp.x + 20.0f + wKind + 8.0f, ty);
                DrawCompLabel(pdl, lp, it.label, mpos, textCol, matchCol);
                if (!it.hint.empty()) {
                    float lw = ImGui::CalcTextSize(it.label.c_str()).x;
                    pdl->AddText(font, fs, ImVec2(lp.x + lw + 8.0f, ty), hintCol, TrimSig(it.hint).c_str());
                }
                if (clicked) { m_compIdx = i; AcceptCompletion(); break; }
            }
            ImGui::PopStyleVar();
            if (total > visible)
                ImGui::TextDisabled("... 共 %d 项 (↑↓ 滚动)", total);
            // AcceptCompletion may clear the candidate vector during this frame.
            if (detailsH>0 && m_compOpen && m_compIdx>=0 && m_compIdx<(int)m_compItems.size()) {
                ImGui::Separator();
                ImGui::BeginChild("##completion_details",ImVec2(0,detailsH),ImGuiChildFlags_None,ImGuiWindowFlags_NoFocusOnAppearing);
                if(hasDocumentation) g_app.plugins.DrawCompletionDetails(*this);
                else { ImGui::PushTextWrapPos(0);ImGui::TextUnformatted(m_compItems[m_compIdx].hint.c_str());ImGui::PopTextWrapPos(); }
                ImGui::EndChild();
            }
            // An interactive details window must share the normal popup layer:
            // tooltips render above combo lists and would hide the source picker.
            // Return typing focus after scrolling/selecting documentation.
            auto* completionWindow=ImGui::GetCurrentWindow();
            auto* focused=ImGui::GetCurrentContext()->NavWindow;
            if(focused && focused->RootWindow==completionWindow && !io.MouseDown[0] &&
               !ImGui::IsPopupOpen(nullptr,ImGuiPopupFlags_AnyPopupId)) {
                ImGui::FocusWindow(editorHost);
                ImGui::BringWindowToDisplayFront(completionWindow);
            }
        }
        ImGui::End();
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(3);
    }

    // ---- 参数签名提示 (Calltip) ----
    if (m_calltipOpen && !m_calltipSig.empty()) {
        float cx = rect.Min.x + gutter + CharX(m_calltipParen) - m_scrollX;
        float cyLine = rect.Min.y + cursorRow * lineH - m_scrollY;
        float tipH = ImGui::GetFrameHeightWithSpacing() + 8;
        float cy = (cyLine - tipH > rect.Min.y) ? cyLine - tipH : cyLine + lineH + 4;
        ImGui::SetNextWindowPos(ImVec2(cx, cy));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6, 3));
        if (ImGui::Begin("##calltip", nullptr,
                         ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoFocusOnAppearing |
                         ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoCollapse |
                         ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_Tooltip)) {
            // 渲染 name(arg1, arg2): 拆分签名, 高亮当前参数
            const std::string& sig = m_calltipSig;
            bool closed = !sig.empty() && sig.back() == ')';
            size_t op = sig.find('(');
            if (op == std::string::npos) {
                ImGui::TextUnformatted(sig.c_str());
            } else {
                size_t cl = closed ? sig.size() - 1 : sig.size();
                ImGui::TextUnformatted(sig.substr(0, op + 1).c_str());
                std::vector<std::string> args;
                int depth = 0; std::string cur;
                for (size_t i = op + 1; i < cl; i++) {
                    char c = sig[i];
                    if (c == '(' || c == '[') depth++;
                    else if (c == ')' || c == ']') depth--;
                    if (c == ',' && depth == 0) { args.push_back(cur); cur.clear(); }
                    else cur += c;
                }
                if (!cur.empty() || args.empty()) args.push_back(cur);
                for (int i = 0; i < (int)args.size(); i++) {
                    std::string a = args[i];
                    size_t b2 = a.find_first_not_of(" \t");
                    size_t e2 = a.find_last_not_of(" \t");
                    a = (b2 == std::string::npos) ? "" : a.substr(b2, e2 - b2 + 1);
                    ImGui::SameLine(0, 0);
                    if (i == m_calltipArg) ImGui::TextColored(ImVec4(1, 0.85f, 0.4f, 1), "%s", a.c_str());
                    else ImGui::TextUnformatted(a.c_str());
                    if (i + 1 < (int)args.size()) { ImGui::SameLine(0, 0); ImGui::TextUnformatted(","); }
                }
                if (closed) { ImGui::SameLine(0, 0); ImGui::TextUnformatted(")"); }
            }
        }
        ImGui::End();
        ImGui::PopStyleVar();
    }
    ImGui::PopFont();
}
