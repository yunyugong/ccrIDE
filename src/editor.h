#pragma once
// MiniCPP - 代码编辑器 (ImGui 自绘, 支持高亮/补全/查找/撤销)
#include <string>
#include <vector>
#include <set>
#include <unordered_map>
#include <cstdint>
#include <imgui.h>
#include "code_model.h"
#include <memory>
#include "symbol_index.h"

// 注意: 不要命名为 TokenType —— Windows SDK winnt.h 中已有同名枚举器, 会冲突
enum class TokenKind : uint8_t { Default, Keyword, String, Comment, Number, Preprocessor, TypeName };

struct Token { size_t start, len; TokenKind type; };

// 一条编译诊断 (自动语法检查 / F5 编译)
struct Diag { int line = 0, col = 0; bool isErr = true; std::string msg; };

class CodeEditor {
public:
    std::string  m_name;    // 标签显示名
    std::wstring m_path;    // 完整路径 (空 = 未命名)
    bool m_dirty = false;   // 未保存修改
    static uint64_t s_uidCounter;
    uint64_t m_uid = ++s_uidCounter;   // 打开序唯一 ID (符号索引会话 key)

    // ---- 文本 ----
    std::string m_text;
    std::string m_savedBaseline;
    bool m_lineChangesDirty = true;
    std::vector<bool> m_originalLines;
    std::vector<int> m_breakpoints;
    void RefreshLineChanges();
    void ToggleBreakpoint(int line);
    int m_enc = 0;        // 文件编码: 0=UTF-8 (默认), 1=GBK (保存时保持原编码)
    std::vector<size_t> m_lines;   // 每行起始 byte 偏移
    bool m_linesDirty = true;

    // ---- 光标 / 选区 ----
    size_t m_cursor = 0;
    size_t m_selAnchor = 0;
    bool   m_selActive = false;
    bool m_mouseSelecting = false;
    bool m_wordSelecting = false;
    size_t m_wordAnchorA = 0, m_wordAnchorB = 0;
    size_t m_lineAnchor = 0;
    bool m_lineSelecting = false;   // 行号栏发起的整行选择

    struct Caret {size_t anchor=0,cursor=0;};
    std::vector<Caret> m_carets;
    bool m_columnSelecting=false;float m_columnX=0,m_columnY=0;
    void AddCaret(size_t offset);
    void ColumnSelect(float x1,float y1,float x2,float y2);
    void MultiReplace(const std::string& text,int deletion=0,bool distribute=false);
    bool MultiKeyboard();
    // ---- 滚动 (像素) ----
    float m_scrollX = 0, m_scrollY = 0;
    bool  m_cursorMoved = true;

    // ---- 撤销 / 重做 ----
    struct UndoOp { size_t pos; std::string removed, inserted; };
    std::vector<UndoOp> m_undo, m_redo;
    static constexpr int MAX_UNDO = 800;

    // ---- 补全 ----
    struct CompSource { std::wstring file; int line = 1; std::string doc; };
    struct CompItem {
        std::string label;      // 补全文本
        const char* kind = "";  // 关键字/类型/函数/变量/成员/对象/头文件
        std::string hint;       // 灰色辅助说明 (签名/所属类型等)
        bool func = false;      // 函数: 接受时自动补 "()"
        std::wstring fullPath;  // #include 文件补全: 完整路径 (使用频率统计)
        bool isDir = false;     // #include 文件补全: 目录项 (接受后继续下钻)
        std::string doc;        // 声明附近的注释 (补全弹窗右侧/下方展示, 类似 VSCode)
        std::vector<CompSource> sources;
        int sourceIndex = 0;
        unsigned long long languageRevision=0;
        size_t languageOffset=0,replaceStart=0,replaceEnd=0;
        std::string insertion;
    };
    bool m_compOpen = false;
    bool m_languagePending = false;
    std::vector<CompItem> m_compItems;
    std::string m_compPrefix;
    size_t m_compStart = 0;
    int    m_compIdx = 0;
    int    m_compScroll = 0;        // 候选列表首个可见项 (超过一屏时滚动)
    bool   m_compTouched = false;   // 用户按过 ↑↓ / 悬停选定过候选 (空前缀时 Tab/Enter 才接受)

    // ---- 词法补全候选池 (会话级缓存) ----
    // 关键字/类型/常用名/所有打开文档标识符合并去重后的候选集合。补全会话开始
    // (OpenCompletion) 或打开的标签集合变化时重建, 输入过程中直接复用, 避免每个
    // 按键都重新收集全部文档标识符、重复做大小写转换与全量排序。
    struct CompPoolEntry {
        std::string label;
        const char* kind;       // 指向静态字面量
        std::string hint;
        bool   func;
        bool   doc;             // 来源: 文档标识符 (用于"排除正在输入的词自身")
        int    base;            // 来源加成 + 使用频率加成 (不含前缀匹配分)
        std::string lower;      // label 的小写 (预计算)
        std::string initials;   // 首字母缩写小写 (预计算)
    };
    std::vector<CompPoolEntry> m_wordPool;
    bool     m_wordPoolDirty = true;
    uint64_t m_wordPoolEdSig = 0;   // 池对应的"打开标签集合"签名
    void EnsureWordPool();          // 重建/复用词法候选池

    // ---- 文档级语义模型 (作用域感知补全 / 对象类型推断 / 用户类型成员) ----
    bool m_decorationDirty = true;
    int m_decorationTabWidth = 0;
    std::vector<int> m_bracketLevel;
    std::vector<int> m_indentColumns;
    void RebuildDecorations();
    ImU32 SyntaxColor(const Token& token, size_t lineStart, size_t lineEnd) const;
    mutable CodeModel m_code;
    void EnsureCodeModel() const { m_code.Ensure(m_text); }
    const CodeModel& Code() const { EnsureCodeModel(); return m_code; }

    // ---- 头文件语义模型 (解析本文件 #include 的头文件, 递归内联; 仿 CppPreprocessor) ----
    mutable CodeModel m_hdr;
    mutable SymbolIndex::HeaderUnit m_hdrUnit;   // 翻译单元文本 + 行来源映射 (跳转定义用)
    uint64_t m_textRevision=0;
    mutable uint64_t m_headerHashRevision=~uint64_t(0),m_headerTextHash=0;
    mutable bool m_headerPending=false;
    mutable bool     m_hdrValid = false;
    mutable uint64_t m_hdrSig = 0;   // 对应的 #include 集合签名
    void EnsureHeaderModel() const;

    // ---- #include 文件补全 (仿 RedPanda: 本地目录 + include 搜索路径 + 目录下钻 + 使用频率排序) ----
    struct FileCompEntry {
        std::string  name;      // 文件名 (目录不含尾部 '/')
        std::wstring fullPath;  // 完整路径
        int          type = 0;  // 0=本地目录 (引号)  1=系统/include 路径 (尖括号)
        bool         isDir = false;
    };
    std::vector<FileCompEntry> m_fileCompCache;               // 当前目录层级的候选缓存
    bool m_includeDirsPending=false;
    std::string m_fileCompKey;                                // 缓存键 (来源+目录前缀+文件)
    std::unordered_map<std::wstring,int> m_fileUsage;         // fullPath -> 使用次数
    bool m_fileUsageLoaded = false;
    std::unordered_map<std::string,int> m_symUsage;           // 符号名 -> 使用次数 (全局, 跨文件)
    bool m_symUsageLoaded = false;
    void LoadFileUsage();
    void SaveFileUsage();
    void BumpFileUsage(const std::wstring& fullPath);
    void LoadSymUsage();
    void SaveSymUsage();
    void BumpSymUsage(const std::string& name);
    int  SymUsageBonus(const std::string& name);              // 使用频率加成 (0..80)
    void GatherHeaderCandidates(const std::string& phrase, bool localQuote, std::vector<CompItem>& out);
    bool InIncludeCompletion(size_t& pathStart, bool& isQuote) const;  // 光标在未闭合 #include 路径内?
    size_t CompPrefixStart(bool& isInclude, bool& isQuote) const;      // 补全前缀起点 (include 时含路径符号)

    // ---- 查找 / 替换 / 跳转 ----
    bool m_findOpen = false;
    std::string m_findStr;
    std::vector<size_t> m_findMatches;
    int m_findIdx = -1;
    std::string m_replaceStr;
    bool m_findCaseSensitive = false;
    bool m_findWholeWord = false;
    bool m_gotoOpen = false;
    bool m_gotoFocus = false;
    int  m_gotoLine = 0;

    // ---- 诊断 (自动语法检查结果, line 为 1-based) ----
    std::vector<Diag> m_diags;

    // ---- 渲染缓存 (编辑时失效, Draw 时重建, 避免每帧全量重算) ----
    mutable float m_maxLineWidth = 0;
    mutable ImFont* m_widthFont = nullptr;
    mutable float m_widthFontSize = 0;
    int m_widthTabWidth = 0;
    mutable bool   m_maxLineDirty = true;
    mutable std::vector<std::vector<Token>> m_tokCache;   // 每行分词缓存
    mutable std::vector<bool> m_tokValid;

    // ---- 参数签名提示 (Calltip) ----
    bool        m_calltipOpen = false;
    std::string m_calltipSig;    // "printf(const char* fmt, ...)"
    size_t      m_calltipParen = 0;  // "(" 的偏移
    int         m_calltipArg = 0;    // 当前参数序号 (按顶层逗号实时计算)

    // ---- 标识符缓存 ----
    std::set<std::string> m_idents;                        // 全部标识符
    std::unordered_map<std::string, int> m_identFreq;      // 使用次数 (排序用)
    std::set<std::string> m_funcIdents;                    // 后跟 "(" 的标识符 -> 判定为函数
    bool m_identsDirty = true;

    // ---- 块注释缓存 ----
    std::vector<bool> m_lineInComment;
    bool m_commentDirty = true;

    // ---- 代码折叠 (仅显示层隐藏; 编辑/光标落入折叠区自动展开) ----
    std::vector<std::pair<int, int>> m_folds;  // {锚行, 最后隐藏行}; 隐藏行 = [锚+1, 最后]
    mutable std::vector<int> m_visLine;        // 可视行缓存: 行号(0-based) -> 逻辑行
    mutable bool m_visDirty = true;
    mutable std::vector<int> m_foldCands;      // 可折叠锚行缓存 (行尾 '{' 等)
    mutable bool m_foldCandsDirty = true;
    void  InvalidateFolds();                    // 任何编辑后调用: 全部展开(简单安全)
    void  RebuildVisCache() const;              // 重建可视行缓存
    int   VisibleRowOfLine(int line) const;     // 逻辑行 -> 可视行 (-1 若被折叠)
    int   LineOfVisualRow(int row) const;       // 可视行 -> 逻辑行
    float VisualY(int line) const;              // 逻辑行顶部内容 y
    bool  IsLineHidden(int line) const;         // 该逻辑行是否被某折叠隐藏
    void  ExpandFoldContaining(int line);       // 展开包含该行的折叠
    void  ToggleFoldAt(int line);               // 在锚行展开/折叠
    void  BuildFoldCandidates() const;          // 重建候选锚行列表
    int   FindFoldEnd(int start) const;         // 括号配平找折叠终点 (找不到返回 -1)

    // ---- 本帧绘制区域 (屏幕坐标) ----
    ImVec2 m_drawPos, m_drawSize;
    bool   m_mouseInEditor = false;

    // ================= 基本 =================
    void SetText(const std::string& t);
    void SetPath(const std::wstring& p);
    bool HasPath() const { return !m_path.empty(); }
    bool Save();
    bool SaveAs();
    bool SaveToPath(const std::wstring& p);
    std::string GetSelectionText() const;
    size_t SelBegin() const { return m_selActive ? std::min(m_selAnchor, m_cursor) : m_cursor; }
    size_t SelEnd()   const { return m_selActive ? std::max(m_selAnchor, m_cursor) : m_cursor; }

    // ================= 度量 =================
    float LineH() const;        // 行高
    float CharW() const;        // 等宽字符宽
    float GutterW() const;      // 行号栏宽

    // ================= 编辑 =================
    void InsertAt(size_t pos, const std::string& s, bool record = true);
    void EraseRange(size_t a, size_t b, bool record = true);  // [a,b)
    void DeleteSelection();
    void InsertText(const std::string& s);    // 插入(替换选区), 记录撤销
    void DoBackspace();
    void DoDelete();
    void DoEnter();                           // 智能缩进
    int TabWidth() const;
    std::string IndentUnit() const;
    bool ShouldAutoComplete() const;
    void MultiIndent(bool newline);
    void DoTab();                             // 4 空格
    void DoAutoBracket(unsigned int c);       // 自动配对
    void Undo();
    void Redo();
    void MoveLeft(bool sel, bool word);
    void MoveRight(bool sel, bool word);
    void MoveUp(bool sel);
    void MoveDown(bool sel);
    void MoveHome(bool sel, bool ctrl);
    void MoveEnd(bool sel, bool ctrl);
    void PageUp(bool sel);
    void PageDown(bool sel);
    void SelectAll();

    // ---- 位置计算 ----
    size_t PrevChar(size_t pos) const;
    size_t NextChar(size_t pos) const;
    size_t WordLeft(size_t pos) const;
    size_t WordRight(size_t pos) const;
    size_t LineOf(size_t pos) const;
    size_t LineStart(size_t line) const;
    size_t LineEnd(size_t line) const;         // 不含 \n
    size_t VisualCol(size_t pos) const;        // 行内字符列 (UTF-8 码点数)
    size_t PosAtLineCol(size_t line, size_t col) const;
    size_t ColToByte(size_t line, size_t col) const;
    void ClampCursor();
    size_t HitTest(float contentX, float contentY) const;  // 内容坐标 -> 偏移
    float  CharX(size_t pos) const;            // 该偏移相对行首的像素 x
    void   GotoLine(int line);                 // 跳转到行 (1-based, 供问题列表点击)
    int    FindMatchingBracket(size_t pos) const;  // 括号匹配: 返回匹配偏移 / -1
    std::string IdentAt(size_t pos);               // 取 pos 处(或前)的标识符
    bool   GotoDefinition(bool useLanguage = true); // 跳转到光标下标识符的定义
    void   ToggleBookmark();                       // 当前行加/解书签
    void   NextBookmark();                         // 跳到下一个书签 (循环)
    void   PrevBookmark();                         // 跳到上一个书签 (循环)
    void   ClearAllBookmarks();                    // 清除所有书签
    bool   HasBookmark(int line) const;            // 某行是否有书签 (0-based)
    std::vector<int> m_bookmarks;                  // 书签行 (0-based, 升序)

    // ================= 渲染 / 交互 =================
    void Draw();            // 每帧, 占据当前窗口内容区
    void DrawFindBar();
    void DrawGotoBar();
    void ReplaceOne();
    void ReplaceAll();
    void DrawContextMenu();

    // ================= 补全 =================
    void OpenCompletion();
    void UpdateCompletion();
    bool AcceptCompletion();   // 返回是否真的接受了候选 (false = 放行给 Tab/Enter 原功能)
    void CloseCompletion();
    void CollectIdents();
    // 上下文感知补全: 词/成员(. ->)/命名空间(::)/头文件(#include), 前缀+模糊子序列匹配
    void GatherCandidates(const std::string& prefix, std::vector<CompItem>& out);
    void SetCompletionSources(CompItem& item, const std::string& src, size_t pos,
                              const CodeModel* model = nullptr, const CSym* symbol = nullptr) const;
    bool CursorInStringOrComment() const;                        // 光标是否在字符串/注释里

    // ================= 参数签名提示 =================
    bool TryOpenCalltip();                     // 输入 "(" 时尝试弹出
    void CloseCalltip() { m_calltipOpen = false; }
    bool FindSignature(const std::string& name, const std::string& objType, std::string& sig);
    int  CountTopCommas(size_t a, size_t b) const;   // 顶层逗号数 (排除括号内)

    // ================= 查找 =================
    void RecomputeMatches();
    void FindNext(bool back);

    // ================= 内部 =================
    void RebuildLines();
    void RebuildCommentCache();
    void EnsureTokens(size_t line) const;           // 构建某行的分词缓存 (供渲染/括号匹配共用)
    bool IsPosInStringOrComment(size_t pos) const;  // 偏移是否落在字符串/注释 token 内
    void PushUndo(size_t pos, const std::string& removed, const std::string& inserted);
    void EnsureCursorVisible();
    void InvalidateAll();
    std::vector<Token> TokenizeLine(size_t line, bool& inBlockComment) const;
    bool IsIdentChar(char c) const;
    bool IsIdentStart(char c) const;
};
