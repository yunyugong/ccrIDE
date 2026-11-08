#include <fstream>
#include <windows.h>
#include <tlhelp32.h>
#include "language_host.h"
#include "lsp_support.h"
#include "../plugins/completion-docs/comment_links.h"
#include "../plugins/completion-docs/completion_docs.h"
#include "plugin_test_support.h"
#include <cmath>
#include <nlohmann/json.hpp>
#include <imgui_internal.h>
// MiniCPP - 纯逻辑回归测试 (不依赖 GLFW/窗口)
// 覆盖: utils 编码/哈希/引号、Console 环形缓冲上限、编辑器文本/撤销/书签/查找/
//       全部替换/分词/声明类型推导、设置 JSON 往返、procStatus 线程安全访问器。
// 用法:
//   g++ -std=c++17 -O1 -IE:\cPP\imgui -IE:\cPP\imgui\backends -IE:\cPP\nlohmann -Isrc \
//       E:\cPP\imgui\imgui.cpp E:\cPP\imgui\imgui_draw.cpp E:\cPP\imgui\imgui_tables.cpp \
//       E:\cPP\imgui\imgui_widgets.cpp \
//       src\utils.cpp src\config.cpp src\app.cpp src\editor.cpp src\symbol_index.cpp \
//       src\ai_assist.cpp src\plugin_manager.cpp tests\run_tests.cpp \
//       -o build\run_tests.exe -luser32 -lgdi32 -lshell32 -lwininet -lcomdlg32 -limm32 \
//       -lole32 -ladvapi32 -lshlwapi
//   返回 0 = 全部通过; 非 0 = 有失败
#include <cstdio>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <filesystem>
#include "i18n.h"
#include "community.h"
#include "app.h"          // g_app / Console / App
#include "editor.h"
#include "symbol_index.h"
#include "utils.h"
#include "portable_paths.h"
#include "line_changes.h"

static int g_fail = 0;
static int g_pass = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (cond) { g_pass++; }                                            \
        else { g_fail++;                                                   \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); }  \
    } while (0)

#define CHECK_EQ(a, b)                                                     \
    do {                                                                   \
        auto va = (a); auto vb = (b);                                      \
        if (va == vb) { g_pass++; }                                        \
        else { g_fail++;                                                   \
            std::printf("FAIL %s:%d  %s == %s\n", __FILE__, __LINE__, #a, #b); } \
    } while (0)

static const char* Utf8Zh() { return "\xE4\xB8\xAD\xE6\x96\x87"; }        // 中文
static const char* GbkZh()  { return "\xD6\xD0\xCE\xC4"; }                // GBK 编码的"中文"

static void TestUtils() {
    // ---- SHA-256 标准向量 (FIPS 180-4) ----
    CHECK_EQ(utils::Sha256Hex(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK_EQ(utils::Sha256Hex("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

    // ---- UTF-8 <-> UTF-16 / GBK 往返 ----
    std::string zh(Utf8Zh());
    std::wstring w = utils::U8ToW(zh);
    CHECK_EQ(utils::WToU8(w), zh);                              // U8->W->U8 无损
    std::string g = utils::Utf8ToGbk(zh);
    CHECK_EQ(g, std::string(GbkZh()));                          // UTF-8 中文 -> GBK 字节
    CHECK_EQ(utils::GbkToUtf8(g), zh);                          // GBK -> UTF-8 还原

    // ---- 严格 UTF-8 校验 ----
    CHECK(utils::IsValidUtf8(zh));
    CHECK(utils::IsValidUtf8("plain ascii"));
    std::string bad = "\x80\x80\xFF";                           // 非法续字节序列
    CHECK(!utils::IsValidUtf8(bad));

    // ---- 命令行引号 ----
    CHECK_EQ(utils::QuoteArg(L"abc"), L"\"abc\"");
    CHECK_EQ(utils::QuoteArg(L"a\"b"), L"\"a\\\"b\"");
    CHECK_EQ(utils::QuoteArg(L"end\\"), L"\"end\\\\\"");
    CHECK_EQ(portable::Resolve(L"C:\\MiniCPP", L"toolchain/mingw64/bin/g++.exe"), L"C:\\MiniCPP\\toolchain\\mingw64\\bin\\g++.exe");
    CHECK_EQ(portable::Store(L"C:\\MiniCPP", L"C:\\MiniCPP\\toolchain\\mingw64\\bin\\g++.exe"), L"toolchain/mingw64/bin/g++.exe");
    CHECK_EQ(portable::Store(L"C:\\MiniCPP", L"D:\\other\\g++.exe"), L"D:\\other\\g++.exe");
    CHECK_EQ(portable::Resolve(L"C:\\MiniCPP", L"D:\\other\\g++.exe"), L"D:\\other\\g++.exe");
}

static void TestConsoleCap() {
    Console c;
    for (int i = 1; i <= 7000; i++)          // 超过上限 1000 行
        c.Append(std::to_string(i) + "\n", 0xFFFFFFFFu);
    CHECK_EQ(c.lines.size(), (size_t)Console::MAX_LINES);   // 7000 只保留最后 6000
    CHECK_EQ(c.lines.front().text, std::string("1001\n"));  // 保留的是最"新"的 (队列语义)
    CHECK_EQ(c.lines.back().text, std::string("7000\n"));
    c.Clear();
    CHECK_EQ(c.lines.size(), (size_t)0);
    c.Append(std::string(Console::MAX_BYTES + 1024, 'x'), 0xFFFFFFFFu);
    CHECK(c.textBytes <= Console::MAX_BYTES);
    c.Append("latest status", 0xFFFFFFFFu);
    CHECK(c.textBytes <= Console::MAX_BYTES);
    CHECK_EQ(c.lines.back().text, std::string("latest status"));
    App queued;
    queued.QueueProcOutput(std::string(2 * 1024 * 1024, 'x'), 0xFFFFFFFFu);
    queued.QueueProcOutput("terminal status", 0xFFFFFFFFu);
    CHECK(queued.queuedOutputBytes <= 1024 * 1024);
    CHECK(queued.outputTruncated);
    CHECK_EQ(queued.procOutputs.back().back().text, std::string("terminal status"));
}

static void TestLineChanges() {
    CHECK(BaselineLines("a\nb\nc","a\nnew\nb\nc")==std::vector<bool>({true,false,true,true}));
    CHECK(BaselineLines("a\nb\nc","a\nchanged\nc")==std::vector<bool>({true,false,true}));
    CHECK(BaselineLines("a\nb\nc","a\nc")==std::vector<bool>({true,true}));
    CodeEditor editor; editor.SetText("a\nb\nc"); editor.RebuildLines();
    editor.m_cursor=2; editor.InsertText("new\n"); editor.RefreshLineChanges();
    CHECK(editor.m_originalLines==std::vector<bool>({true,false,true,true}));
    editor.Undo(); editor.RefreshLineChanges(); CHECK(editor.m_originalLines==std::vector<bool>({true,true,true}));
    editor.Redo(); editor.RefreshLineChanges(); CHECK(!editor.m_originalLines[1]);
    auto path=std::filesystem::temp_directory_path()/(L"ccride-line-save-"+std::to_wstring(GetCurrentProcessId())+L".cpp");
    CHECK(editor.SaveToPath(path.wstring())); editor.RefreshLineChanges();
    CHECK(std::all_of(editor.m_originalLines.begin(),editor.m_originalLines.end(),[](bool original){return original;}));
    std::filesystem::remove(path);
    std::string oldText,newText; for(int i=0;i<2000;++i) {oldText+="old"+std::to_string(i)+"\n";newText+="new"+std::to_string(i)+"\n";}
    auto large=BaselineLines(oldText,newText); CHECK(large.size()==2001&&!large[1000]&&large.back());
}

static void TestEditorBasics() {
    CodeEditor ed;
    // 无末尾换行的三段文本; 行起始偏移全部由引擎计算, 断言用 find() 定位避免手算
    const std::string text = "int a = 1;\nint b = 2;\n\nint c = 3;";
    ed.SetText(text);
    ed.RebuildLines();
    CHECK_EQ(ed.LineOf(ed.m_text.size()), ed.LineOf(ed.m_text.size() - 1));  // 光标在文本尾 -> 末行
    CHECK(ed.LineEnd(2) == 0 || ed.LineEnd(2) >= ed.LineStart(2));           // 行区间合法

    size_t pB = text.find("int b");
    CHECK(ed.LineOf(pB) == 1);                    // 第二行
    CHECK(ed.LineStart(1) == pB);
    CHECK(ed.LineEnd(1) == text.find("\n\n"));    // 第二行行尾不含 '\n' 且指向空行行首

    // 光标在字符间的 UTF-8 安全移动 (ASCII 文本退化为逐字节)
    CHECK(ed.PrevChar(3) == 2);
    CHECK(ed.NextChar(0) == 1);

    // 插入换行 -> 行表重建 & 后续逻辑行随之下移
    size_t before = ed.m_lines.size();
    ed.InsertAt(pB, "int inserted = 9;\n");
    CHECK_EQ(ed.m_lines.size(), before + 1);
    CHECK(ed.m_text.find("int inserted") == pB);
    // 撤销一次回到原文
    ed.Undo();
    CHECK_EQ(ed.m_text, text);

    // 标识符收集: 函数名判定
    CodeEditor f;
    f.SetText("void myfunc() { int localVar = 1; myfunc(); }\n");
    f.RebuildLines();
    f.CollectIdents();
    CHECK(f.m_idents.count("myfunc") > 0);
    CHECK(f.m_idents.count("localVar") > 0);
    CHECK(f.m_funcIdents.count("myfunc") > 0);
    CHECK(f.m_funcIdents.count("localVar") == 0);   // 不后跟 '('
}

static void TestEditorUndoRedo() {
    CodeEditor ed;
    const std::string text = "hello world";
    ed.SetText(text);
    ed.RebuildLines();

    // SetText 将光标重置到 0; 模拟用户在文尾输入需先把光标移到末尾
    ed.m_cursor = ed.m_text.size();
    ed.m_selAnchor = ed.m_cursor;
    ed.InsertText("!!!");
    CHECK_EQ(ed.m_text, "hello world!!!");
    ed.Undo();
    CHECK_EQ(ed.m_text, text);
    ed.Redo();
    CHECK_EQ(ed.m_text, "hello world!!!");

    // 选中删除
    ed.m_selAnchor = 0; ed.m_cursor = 5; ed.m_selActive = true;
    ed.DeleteSelection();
    CHECK_EQ(ed.m_text, " world!!!");
    ed.Undo();
    CHECK_EQ(ed.m_text, "hello world!!!");

    // 文本中部插入后重做: 不能吞掉插入点之后的字符
    // (回归: redo 栈的 removed 字段曾误存为 inserted, 重做时按 inserted 长度多删了一截)
    CodeEditor ed2;
    ed2.SetText("abc");
    ed2.RebuildLines();
    ed2.m_cursor = 1; ed2.m_selAnchor = 1; ed2.m_selActive = false;
    ed2.InsertText("X");
    CHECK_EQ(ed2.m_text, "aXbc");
    ed2.Undo();
    CHECK_EQ(ed2.m_text, "abc");
    ed2.Redo();
    CHECK_EQ(ed2.m_text, "aXbc");
    CHECK_EQ(ed2.m_cursor, (size_t)2);

    // 替换 (removed/inserted 均非空) 的撤销/重做往返
    CodeEditor ed3;
    ed3.SetText("foo bar");
    ed3.RebuildLines();
    ed3.EraseRange(0, 3, true);      // "foo bar" -> " bar"
    ed3.InsertAt(0, "baz", true);    // " bar" -> "baz bar"
    CHECK_EQ(ed3.m_text, "baz bar");
    ed3.Undo();
    CHECK_EQ(ed3.m_text, " bar");
    ed3.Undo();
    CHECK_EQ(ed3.m_text, "foo bar");
    ed3.Redo();
    CHECK_EQ(ed3.m_text, " bar");
    ed3.Redo();
    CHECK_EQ(ed3.m_text, "baz bar");

    // 撤销后再编辑: 重做栈必须清空
    ed3.Undo();                       // "baz bar" -> " bar", 此时重做栈非空
    CHECK_EQ(ed3.m_text, " bar");
    ed3.InsertAt(ed3.m_text.size(), "!", true);
    CHECK_EQ(ed3.m_text, " bar!");
    ed3.Redo();                       // 新编辑已作废重做历史
    CHECK_EQ(ed3.m_text, " bar!");

    // 选区被输入替换 -> 单步撤销即恢复原文 (回归: 曾拆成删除/插入两步)
    CodeEditor ed4;
    ed4.SetText("hello");
    ed4.RebuildLines();
    ed4.m_selAnchor = 0; ed4.m_cursor = 5; ed4.m_selActive = true;
    ed4.InsertText("X");
    CHECK_EQ(ed4.m_text, "X");
    CHECK_EQ(ed4.m_cursor, (size_t)1);
    ed4.Undo();
    CHECK_EQ(ed4.m_text, "hello");
    ed4.Redo();
    CHECK_EQ(ed4.m_text, "X");
}

static void TestBookmarkShift() {
    CodeEditor ed;
    const std::string text = "a\nb\nc\nd\ne";
    ed.SetText(text);
    ed.RebuildLines();
    // 行 3 ("d") 加书签
    ed.m_bookmarks = { 3 };
    // 在行 1 前插入两行 -> 书签应移到 5
    ed.InsertAt(ed.LineStart(1), "x\ny\n");
    CHECK(ed.HasBookmark(5));
    // 删除这插入的两行 -> 书签回到 3
    ed.EraseRange(ed.LineStart(1), ed.LineStart(3));
    CHECK(ed.HasBookmark(3));

    // 删除包含书签行的区间 (第 1~2 行, 书签在第 2 行) -> 书签被移除
    ed.m_bookmarks = { 2 };                    // 行 "c"
    ed.EraseRange(ed.LineStart(1), ed.LineStart(3));  // 删掉 "b"/"c" 两行, 区间覆盖书签行
    CHECK(ed.m_bookmarks.empty());
}

static void TestFindReplace() {
    CodeEditor ed;
    const std::string text = "Foo foo foobar foo\nfoo";
    ed.SetText(text);
    ed.RebuildLines();

    // 大小写不敏感
    ed.m_findCaseSensitive = false; ed.m_findWholeWord = false;
    ed.m_findStr = "foo";
    ed.RecomputeMatches();
    CHECK_EQ(ed.m_findMatches.size(), (size_t)5);   // Foo+foo+foobar+foo+foo

    // 整词 (排除 foobar 内部)
    ed.m_findWholeWord = true;
    ed.RecomputeMatches();
    CHECK_EQ(ed.m_findMatches.size(), (size_t)4);

    // 大小写敏感 + 整词 (再排除大写 Foo)
    ed.m_findCaseSensitive = true;
    ed.RecomputeMatches();
    CHECK_EQ(ed.m_findMatches.size(), (size_t)3);

    // ReplaceAll: 与撤回合力验证 (每次替换独立入撤销栈)
    CodeEditor r;
    const std::string orig = "aa bb aa cc aa";
    r.SetText(orig);
    r.RebuildLines();
    r.m_findStr = "aa"; r.m_replaceStr = "X";
    r.m_findCaseSensitive = true; r.m_findWholeWord = true;
    r.ReplaceAll();
    CHECK_EQ(r.m_text, std::string("X bb X cc X"));
    while (!r.m_undo.empty()) r.Undo();            // 全部回退
    CHECK_EQ(r.m_text, orig);
}

static void TestTokenizerAndDecl() {
    CodeEditor ed;
    ed.SetText("#include <vector>\nint main() { std::vector<int> v; }\n");
    ed.RebuildLines();
    ed.RebuildCommentCache();

    // 预处理行整体一个 Preprocessor token
    bool inBlock = false;
    auto toks0 = ed.TokenizeLine(0, inBlock);
    CHECK_EQ(toks0.size(), (size_t)1);
    CHECK(toks0[0].type == TokenKind::Preprocessor);

    // 关键字高亮: 能扫出 int / main 所在 token
    bool hasKeyword = false;
    for (auto& t : ed.TokenizeLine(1, inBlock))
        if (t.type == TokenKind::Keyword) hasKeyword = true;
    CHECK(hasKeyword);

    // 声明类型推导: v -> std::vector<int> (当前语义模型接口)
    CodeEditor d;
    d.SetText("std::vector<int> v;\nv.push_back(1);\n");
    d.RebuildLines();
    d.EnsureCodeModel();
    CHECK_EQ(d.m_code.ResolveVarType("v", d.m_text.size() - 2), std::string("std::vector<int>"));

    // 括号匹配
    CodeEditor b;
    b.SetText("foo(bar[1])");
    b.RebuildLines();
    int open = (int)b.m_text.find('(');
    int close = (int)b.m_text.find(')');
    CHECK(b.FindMatchingBracket((size_t)open) == close);
    CHECK(b.FindMatchingBracket((size_t)close) == open);

    // 字符串/注释里的括号不参与匹配
    CodeEditor b2;
    b2.SetText("char c = '('; int x = (1);");
    b2.RebuildLines();
    int inStr = (int)b2.m_text.find('(');        // 引号内那个 '('
    int realOpen = (int)b2.m_text.rfind('(');    // (1) 的 '('
    CHECK(b2.FindMatchingBracket((size_t)inStr) == -1);
    CHECK(b2.FindMatchingBracket((size_t)realOpen) == realOpen + 2);
}

// 数字 token 不应吞掉相邻的 +/- (1+2 必须切成 1, +, 2)
static void TestNumberToken() {
    CodeEditor ed;
    ed.SetText("x = 1+2; y = 1e-3; z = 0xFF;");
    ed.RebuildLines();
    bool inBlock = false;
    std::vector<std::string> nums;
    for (auto& t : ed.TokenizeLine(0, inBlock))
        if (t.type == TokenKind::Number) nums.push_back(ed.m_text.substr(t.start, t.len));
    std::vector<std::string> expect = { "1", "2", "1e-3", "0xFF" };
    CHECK_EQ(nums, expect);
}

// 关闭标签后的 active 索引: 删除 idx 之前的标签后, 当前标签应仍指向原文件
static void TestCloseEditorIndex() {
    App app;
    app.NewFile(); app.NewFile(); app.NewFile();   // 3 个标签, active=2
    CHECK_EQ(app.editors.size(), (size_t)3);

    app.active = 1;                                // 选中第 2 个
    app.RemoveEditorAt(0);                         // 删除第 1 个
    CHECK_EQ(app.active, 1 - 1);                   // 仍指向原来的第 2 个 (现索引 0)
    CHECK_EQ(app.editors.size(), (size_t)2);

    app.active = 1;
    app.RemoveEditorAt(1);                         // 删除当前标签, 后面没有标签
    CHECK_EQ(app.active, 0);
    CHECK_EQ(app.editors.size(), (size_t)1);
}

// 补全候选池: 能召回打开文档中的标识符, 且同一会话内复用不重建、标签集合变化后重建
static void TestCompletionPool() {
    g_app.editors.clear();
    auto doc = std::make_unique<CodeEditor>();
    doc->SetText("int alphaValue = 1; void betaFunc() { alphaValue++; }\n");
    doc->RebuildLines();
    g_app.editors.push_back(std::move(doc));

    CodeEditor ed;
    ed.SetText("alpha");
    ed.RebuildLines();
    ed.m_cursor = ed.m_text.size();
    ed.m_selAnchor = ed.m_cursor;
    ed.OpenCompletion();
    bool hasAlpha = false;
    for (auto& it : ed.m_compItems) if (it.label == "alphaValue") hasAlpha = true;
    CHECK(hasAlpha);
    CHECK(!ed.m_wordPoolDirty);                    // 池已构建

    size_t poolSize = ed.m_wordPool.size();
    ed.m_compItems.clear();
    ed.GatherCandidates("beta", ed.m_compItems);   // 同一会话: 复用缓存
    bool hasBeta = false;
    for (auto& it : ed.m_compItems) if (it.label == "betaFunc") hasBeta = true;
    CHECK(hasBeta);
    CHECK_EQ(ed.m_wordPool.size(), poolSize);      // 未重建

    // 标签集合变化 -> 池应重建并纳入新文档的标识符
    auto doc2 = std::make_unique<CodeEditor>();
    doc2->SetText("int gammaSymbol = 3;\n");
    doc2->RebuildLines();
    g_app.editors.push_back(std::move(doc2));
    ed.m_compItems.clear();
    ed.GatherCandidates("gamma", ed.m_compItems);
    bool hasGamma = false;
    for (auto& it : ed.m_compItems) if (it.label == "gammaSymbol") hasGamma = true;
    CHECK(hasGamma);

    g_app.editors.clear();
}

// #include 文件补全: 本地目录扫描 / 头文件过滤 / 目录下钻 / 大小写不敏感前缀
static void TestFileCompletion() {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring dir = std::wstring(tmp) + L"minicpp_fc";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    auto touch = [&](const wchar_t* n) { utils::WriteFileU8(dir + L"\\" + n, ""); };
    touch(L"foo.h");
    touch(L"bar.hpp");
    touch(L"skip.cpp");                 // 非头文件: 应被过滤
    touch(L"noext");                    // 无扩展名: 视为头文件 (如 <vector>)
    std::filesystem::create_directories(dir + L"\\sub", ec);
    utils::WriteFileU8(dir + L"\\sub\\inner.h", "");

    auto labelPresent = [](CodeEditor& ed, const char* label, bool isDir, bool& found) {
        found = false;
        for (auto& it : ed.m_compItems)
            if (it.label == label && it.isDir == isDir) { found = true; break; }
    };

    CodeEditor ed;
    ed.SetPath(dir + L"\\main.cpp");
    ed.SetText("#include \"");
    ed.RebuildLines();
    ed.RebuildCommentCache();
    ed.m_cursor = ed.m_text.size();
    ed.m_selAnchor = ed.m_cursor;
    ed.OpenCompletion();

    bool f = false;
    labelPresent(ed, "foo.h", false, f);   CHECK(f);
    labelPresent(ed, "bar.hpp", false, f); CHECK(f);
    labelPresent(ed, "noext", false, f);   CHECK(f);
    labelPresent(ed, "sub/", true, f);     CHECK(f);
    labelPresent(ed, "skip.cpp", false, f);CHECK(!f);   // 非头文件不出现

    // 目录下钻: 接受 "sub/" 后应列出 sub/inner.h 且保留目录前缀
    int idx = -1;
    for (int i = 0; i < (int)ed.m_compItems.size(); i++)
        if (ed.m_compItems[i].label == "sub/") { idx = i; break; }
    CHECK(idx >= 0);
    ed.m_compIdx = idx;
    ed.m_compTouched = true;
    ed.AcceptCompletion();
    CHECK_EQ(ed.m_text, std::string("#include \"sub/"));
    labelPresent(ed, "inner.h", false, f); CHECK(f);

    // 前缀过滤 (大小写不敏感)
    CodeEditor ed2;
    ed2.SetPath(dir + L"\\main.cpp");
    ed2.SetText("#include \"FO");
    ed2.RebuildLines();
    ed2.RebuildCommentCache();
    ed2.m_cursor = ed2.m_text.size();
    ed2.m_selAnchor = ed2.m_cursor;
    ed2.OpenCompletion();
    labelPresent(ed2, "foo.h", false, f); CHECK(f);

    // 已完成 (闭合) 的 #include 不应触发"文件补全" (最多走普通词法补全)
    CodeEditor ed3;
    ed3.SetPath(dir + L"\\main.cpp");
    ed3.SetText("#include \"foo.h\"");
    ed3.RebuildLines();
    ed3.RebuildCommentCache();
    ed3.m_cursor = ed3.m_text.size();
    ed3.m_selAnchor = ed3.m_cursor;
    ed3.OpenCompletion();
    bool anyFileComp = false;
    for (auto& it : ed3.m_compItems) {
        if (it.kind && (!strcmp(it.kind, "本地头文件") || !strcmp(it.kind, "系统头文件") ||
                        !strcmp(it.kind, "文件夹")))
            anyFileComp = true;
    }
    CHECK(!anyFileComp);

    std::filesystem::remove_all(dir, ec);
}

static void TestSettingsRoundTrip() {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring path = std::wstring(tmp) + L"minicpp_settings_test.json";

    Settings s;
    s.gppPath = "D:\\mingw\\bin\\g++.exe";
    s.theme = "light";
    s.fontSize = 20;
    s.stdVersion = 2;   // C++14
    s.warnAll = true;
    s.autoCheck = false;
    s.codeCompletion = false;
    s.syntaxHighlight = false;
    s.runTimeLimitMs = 3210; s.runMemoryLimitMB = 128; s.runOutputLimitMB = 3;
    s.compileTimeLimitMs = 9000;
    s.runArguments = "\"two words\""; s.runDirectory = "D:/work";
    s.runEnvironment = "TEST=value";
    s.hotkeys["save"] = "Ctrl+Shift+S";
    s.editorColors["light/Selection"] = IM_COL32(10,20,30,255);
    s.rainbowBrackets=false; s.rainbowIndentLines=true; s.rainbowIndentFill=true;
    s.showIndentGuides=false; s.showWhitespace=true;
    s.Save(path);

    Settings s2;
    CHECK(s2.Load(path));
    CHECK_EQ(s2.gppPath, s.gppPath);
    CHECK_EQ(s2.theme, s.theme);
    CHECK_EQ(s2.fontSize, s.fontSize);
    CHECK_EQ(s2.stdVersion, s.stdVersion);
    CHECK_EQ(s2.warnAll, s.warnAll);
    CHECK(!s2.autoCheck); CHECK(!s2.codeCompletion); CHECK(!s2.syntaxHighlight);
    CHECK_EQ(s2.runTimeLimitMs, 3210); CHECK_EQ(s2.runMemoryLimitMB, 128);
    CHECK_EQ(s2.runOutputLimitMB, 3); CHECK_EQ(s2.compileTimeLimitMs, 9000);
    CHECK_EQ(s2.runArguments, s.runArguments); CHECK_EQ(s2.runDirectory, s.runDirectory);
    CHECK_EQ(s2.runEnvironment, s.runEnvironment);
    CHECK_EQ(s2.hotkeys["save"], std::string("Ctrl+Shift+S"));
    CHECK_EQ(s2.editorColors.at("light/Selection"), IM_COL32(10,20,30,255));
    CHECK(!s2.rainbowBrackets); CHECK(s2.rainbowIndentLines); CHECK(s2.rainbowIndentFill);
    CHECK(!s2.showIndentGuides); CHECK(s2.showWhitespace);
    utils::WriteFileU8(path, "{}");
    Settings legacy;
    CHECK(legacy.Load(path));
    CHECK(legacy.autoCheck && legacy.codeCompletion && legacy.syntaxHighlight);
    CHECK_EQ(legacy.runTimeLimitMs, 60000); CHECK_EQ(legacy.runMemoryLimitMB, 256);
    CHECK_EQ(legacy.runOutputLimitMB, 16);
    utils::WriteFileU8(path, "{\"runTimeLimitMs\":-1,\"runMemoryLimitMB\":-1,\"runOutputLimitMB\":999,\"compileTimeLimitMs\":99999999}");
    CHECK(legacy.Load(path)); CHECK_EQ(legacy.runTimeLimitMs, 100);
    CHECK_EQ(legacy.runMemoryLimitMB, 0); CHECK_EQ(legacy.runOutputLimitMB, 64);
    CHECK_EQ(legacy.compileTimeLimitMs, 3600000);
    DeleteFileW(path.c_str());
}

static void TestCustomization(){
    namespace fs=std::filesystem;
    auto root=fs::temp_directory_path()/(L"ccride-preferences-中文 "+std::to_wstring(GetCurrentProcessId()));
    fs::create_directories(root);auto file=(root/"preferences.json").wstring();std::string error;
    Settings prefs;prefs.tabWidth=2;prefs.insertSpaces=false;prefs.autoIndent=false;prefs.autoCloseBrackets=false;
    prefs.autoCompletion=false;prefs.completionMinChars=3;prefs.completionRows=14;prefs.completionWidth=900;
    prefs.autoCheckDelayMs=2200;prefs.lineHeightPercent=160;prefs.scrollLines=8;
    prefs.showLineNumbers=false;prefs.relativeLineNumbers=true;prefs.highlightCurrentLine=false;
    prefs.highlightWordOccurrences=false;prefs.showChangeMarkers=false;prefs.caretBlink=false;
    prefs.uiFontSize=20;prefs.uiDensity=2;prefs.cornerRadius=9;prefs.scrollbarSize=18;
    prefs.recoveryIntervalSec=20;prefs.showToolbar=false;prefs.showStatusBar=false;prefs.showActivityBar=false;
    prefs.hotkeys["save"]="Ctrl+Alt+S";prefs.editorColors["dark/Selection"]=IM_COL32(1,2,3,255);
    prefs.gppPath="PRIVATE_COMPILER";prefs.runEnvironment="PRIVATE_KEY=secret";
    CHECK(prefs.Save((root/"settings.json").wstring()));
    CHECK(!prefs.Save((root/"missing/settings.json").wstring()));
    Settings loaded;CHECK(loaded.Load((root/"settings.json").wstring()));
    CHECK(loaded.tabWidth==2&&!loaded.insertSpaces&&!loaded.autoIndent&&!loaded.autoCloseBrackets);
    CHECK(!loaded.autoCompletion&&loaded.completionMinChars==3&&loaded.completionRows==14&&loaded.completionWidth==900);
    CHECK(loaded.autoCheckDelayMs==2200&&loaded.lineHeightPercent==160&&loaded.scrollLines==8);
    CHECK(!loaded.showLineNumbers&&loaded.relativeLineNumbers&&!loaded.highlightCurrentLine&&!loaded.highlightWordOccurrences);
    CHECK(!loaded.showChangeMarkers&&!loaded.caretBlink&&loaded.uiFontSize==20&&loaded.uiDensity==2);
    CHECK(loaded.cornerRadius==9&&loaded.scrollbarSize==18&&loaded.recoveryIntervalSec==20);
    CHECK(!loaded.showToolbar&&!loaded.showStatusBar&&!loaded.showActivityBar);
    CHECK(prefs.ExportPreferences(file,error));std::string text;CHECK(utils::ReadFileU8(file,text));
    CHECK(text.find("PRIVATE_")==std::string::npos&&text.find("secret")==std::string::npos);
    auto exported=nlohmann::json::parse(text);CHECK(exported["preferences"].size()==38);
    Settings imported;imported.gppPath="keep-toolchain";imported.runEnvironment="KEEP=value";imported.runTimeLimitMs=1234;
    imported.disabledPlugins={"keep-plugin"};CHECK(imported.ImportPreferences(file,error));
    CHECK(imported.tabWidth==2&&imported.hotkeys==prefs.hotkeys&&imported.editorColors==prefs.editorColors);
    CHECK(imported.gppPath=="keep-toolchain"&&imported.runEnvironment=="KEEP=value"&&imported.runTimeLimitMs==1234);
    CHECK(imported.disabledPlugins==std::vector<std::string>{"keep-plugin"});
    auto invalid=exported;invalid["preferences"]["theme"]="light";invalid["preferences"]["tabWidth"]="oops";
    utils::WriteFileU8(file,invalid.dump());CHECK(!imported.ImportPreferences(file,error));
    CHECK(imported.theme=="dark"&&imported.tabWidth==2&&!error.empty());
    invalid=exported;invalid["preferences"]["editorColors"]["bad"]=-1;utils::WriteFileU8(file,invalid.dump());
    CHECK(!imported.ImportPreferences(file,error));CHECK(imported.editorColors==prefs.editorColors);
    invalid=exported;invalid["preferences"]["tabWidth"]=999;invalid["preferences"]["uiFontSize"]=-5;
    invalid["preferences"]["recoveryIntervalSec"]=0;utils::WriteFileU8(file,invalid.dump());
    CHECK(imported.ImportPreferences(file,error));CHECK(imported.tabWidth==8&&imported.uiFontSize==12&&imported.recoveryIntervalSec==1);
    utils::WriteFileU8(file,"{}");CHECK(!imported.ImportPreferences(file,error));
    utils::WriteFileU8(file,"{bad");CHECK(!imported.ImportPreferences(file,error));
    utils::WriteFileU8(file,std::string(1024*1024+1,'x'));CHECK(!imported.ImportPreferences(file,error));
    imported.ResetPreferences();CHECK(imported.tabWidth==4&&imported.insertSpaces&&imported.autoCompletion&&imported.uiFontSize==15);
    CHECK(imported.hotkeys.empty()&&imported.editorColors.empty());
    CHECK(imported.gppPath=="keep-toolchain"&&imported.runEnvironment=="KEEP=value"&&imported.runTimeLimitMs==1234);
    Settings legacy;utils::WriteFileU8(file,"{}");CHECK(legacy.Load(file));CHECK(legacy.tabWidth==4&&legacy.recoveryIntervalSec==3&&legacy.showToolbar);
    fs::remove_all(root);
    auto saved=g_app.settings;g_app.settings=Settings{};
    ImGui::CreateContext();auto& io=ImGui::GetIO();io.Fonts->AddFontDefault();io.Fonts->Build();
    io.DisplaySize=ImVec2(640,480);io.DeltaTime=1.0f/60;ImGui::NewFrame();
    CodeEditor ed;
    auto setup=[&](const std::string& value,size_t cursor=std::string::npos){ed.SetText(value);ed.RebuildLines();ed.m_cursor=ed.m_selAnchor=cursor==std::string::npos?value.size():cursor;ed.m_selActive=false;};
    g_app.settings.tabWidth=2;setup("a");ed.DoTab();CHECK_EQ(ed.m_text,std::string("a "));
    setup("{}" ,1);ed.DoEnter();CHECK_EQ(ed.m_text,std::string("{\n  \n}"));CHECK(ed.m_cursor==4);
    setup("  \t{");ed.DoEnter();CHECK_EQ(ed.m_text,std::string("  \t{\n      "));
    setup("  ");ed.DoBackspace();CHECK(ed.m_text.empty());
    g_app.settings.insertSpaces=false;setup("");ed.DoTab();CHECK_EQ(ed.m_text,std::string("\t"));
    setup("{}" ,1);ed.DoEnter();CHECK_EQ(ed.m_text,std::string("{\n\t\n}"));
    setup("\t");ed.DoBackspace();CHECK(ed.m_text.empty());
    g_app.settings.autoIndent=false;setup("\t{");ed.DoEnter();CHECK_EQ(ed.m_text,std::string("\t{\n"));
    g_app.settings.autoCloseBrackets=false;setup("");ed.DoAutoBracket('(');CHECK_EQ(ed.m_text,std::string("("));
    setup("()",1);ed.DoBackspace();CHECK_EQ(ed.m_text,std::string(")"));
    g_app.settings.autoCloseBrackets=true;setup("");ed.DoAutoBracket('(');CHECK_EQ(ed.m_text,std::string("()"));CHECK(ed.m_cursor==1);
    for(int width:{2,4,8}){
        g_app.settings.tabWidth=width;setup("\tx\nab\tz");
        CHECK(ed.VisualCol(1)==size_t(width));CHECK(ed.PosAtLineCol(0,width)==1);
        CHECK(std::abs(ed.CharX(1)-ed.CharW()*width)<.1f);
        CHECK(ed.HitTest(ed.CharW()*width+.1f,ed.LineH()*.5f)==1);
        ed.RebuildDecorations();CHECK(ed.m_indentColumns[0]==width);
    }
    g_app.settings.tabWidth=2;ed.RebuildDecorations();CHECK(ed.m_indentColumns[0]==2);
    g_app.settings.autoIndent=true;g_app.settings.insertSpaces=true;setup("a\nab");
    ed.m_carets={{1,1},{4,4}};ed.MultiIndent(false);CHECK_EQ(ed.m_text,std::string("a \nab  "));
    ed.Undo();CHECK_EQ(ed.m_text,std::string("a\nab"));
    setup("  {\n  x");ed.m_carets={{3,3},{7,7}};ed.MultiIndent(true);CHECK_EQ(ed.m_text,std::string("  {\n    \n  x\n  "));
    ed.Undo();CHECK_EQ(ed.m_text,std::string("  {\n  x"));
    g_app.settings.completionMinChars=3;setup("st");CHECK(!ed.ShouldAutoComplete());setup("std");CHECK(ed.ShouldAutoComplete());
    setup("std::");CHECK(ed.ShouldAutoComplete());setup("value.");CHECK(ed.ShouldAutoComplete());
    g_app.settings.autoCompletion=false;CHECK(!ed.ShouldAutoComplete());setup("void f(){ ret");ed.OpenCompletion();CHECK(ed.m_compOpen);
    g_app.settings.codeCompletion=false;ed.OpenCompletion();CHECK(!ed.m_compOpen);
    g_app.settings=Settings{};setup("x");float original=ed.LineH();g_app.settings.lineHeightPercent=180;CHECK(ed.LineH()>original);
    auto normalGutter=ed.GutterW();g_app.settings.showLineNumbers=false;CHECK(ed.GutterW()<normalGutter);
    g_app.settings=saved;ImGui::EndFrame();ImGui::DestroyContext();
}

static void TestIndependentEditorFeatures() {
    const Settings saved = g_app.settings;
    CodeEditor ed;
    ed.SetText("int alphaValue;\nvoid f(){ alph");
    ed.RebuildLines(); ed.m_cursor = ed.m_text.size();
    g_app.settings.codeCompletion = false;
    g_app.settings.syntaxHighlight = true;
    g_app.settings.autoCheck = true;
    ed.OpenCompletion();
    CHECK(!ed.m_compOpen && ed.m_compItems.empty());
    ed.EnsureTokens(0);
    CHECK(!ed.m_tokCache[0].empty());
    Token keyword{0, 3, TokenKind::Keyword};
    CHECK_EQ(ed.SyntaxColor(keyword, 0, ed.LineEnd(0)), g_app.EditorColorValue(EditorColor::TypeKeyword));
    // Bypass popup state to cover stale candidates after a settings change.
    ed.m_compOpen = true;
    ed.m_compItems.push_back({"alphaValue", "variable", "int", false});
    const std::string original = ed.m_text;
    CHECK(!ed.AcceptCompletion()); CHECK_EQ(ed.m_text, original);
    CHECK(!ed.m_compOpen);
    ed.m_calltipOpen = true;
    CHECK(!ed.TryOpenCalltip()); CHECK(!ed.m_calltipOpen);
    g_app.settings.codeCompletion = true;
    g_app.settings.syntaxHighlight = false;
    g_app.settings.autoCheck = false;
    CHECK_EQ(ed.SyntaxColor(keyword, 0, ed.LineEnd(0)), g_app.EditorColorValue(EditorColor::Text));
    ed.OpenCompletion();
    CHECK(ed.m_compOpen);
    bool alpha = false;
    for (const auto& item : ed.m_compItems) if (item.label == "alphaValue") alpha = true;
    CHECK(alpha);
    // Disabling checks must suppress a result already queued by the worker.
    g_app.diagDoneRev = g_app.editRev ? g_app.editRev : 1;
    g_app.diagResults = {{1, 1, true, "stale automatic diagnostic"}};
    const auto before = g_app.activeDiags.size();
    g_app.PumpDiagResults();
    CHECK_EQ(g_app.activeDiags.size(), before);
    CHECK(g_app.diagResults.empty() && g_app.diagDoneRev == 0);
    CHECK(g_app.settings.codeCompletion && !g_app.settings.autoCheck);
    g_app.settings = saved;
}

// procStatus 由后台线程写 / 主线程读 (回归: 旧实现直接读写 std::string 属数据竞争)
static void TestProcStatusThreadSafe() {
    std::atomic<bool> stop{false};
    std::vector<std::thread> writers;
    for (int t = 0; t < 4; t++) {
        writers.emplace_back([&, t]() {
            for (int i = 0; i < 2000 && !stop.load(); i++)
                g_app.SetProcStatus("w" + std::to_string(t) + ":" + std::to_string(i));
        });
    }
    std::thread reader([&]() {
        for (int i = 0; i < 2000 && !stop.load(); i++) {
            std::string s = g_app.GetProcStatus();   // 持锁读取, 不能崩溃/读到撕裂串
            (void)s;
        }
    });
    for (auto& w : writers) w.join();
    stop.store(true);
    reader.join();
    CHECK(g_app.GetProcStatus().find("w") == 0);     // 最终状态为某写线程最后一次写入的完整串
}

static void TestManagedCompileRun() {
    if (g_app.settings.gppPath.empty()) return;
    namespace fs = std::filesystem;
    const auto dir = fs::temp_directory_path() / (L"MiniCPP workflow " + std::to_wstring(GetCurrentProcessId()));
    fs::create_directories(dir);
    const auto source = dir / L"main.cpp";
    const Settings savedSettings = g_app.settings;
    const auto savedArtifacts=g_app.lastArtifacts;
    const auto savedDir = g_app.dataDir;
    auto savedEditors = std::move(g_app.editors);
    const int savedActive = g_app.active;
    g_app.active = -1; g_app.dataDir = dir.wstring();
    g_app.settings.useCmdTemplate = false;
    g_app.settings.extraFlags.clear(); g_app.settings.runArguments.clear();
    g_app.settings.runDirectory.clear(); g_app.settings.runEnvironment.clear();
    g_app.settings.runTimeLimitMs = 1500; g_app.settings.autoCheck = false;
    g_app.NewFile();
    g_app.ActiveEditor()->SetPath(source.wstring());
    auto execute = [&](const std::string& text,BuildAction action=BuildAction::BuildRun) {
        g_app.ActiveEditor()->SetText(text); g_app.MarkDiag();
        g_app.console.Clear(); g_app.terminal.Clear();
        g_app.StartCompileRun(action);
        if (g_app.procThread.joinable()) g_app.procThread.join();
        g_app.PumpProcResults(); g_app.PumpDiagResults();
        std::string logs; for (const auto& line : g_app.console.lines) logs += line.text;
        for (const auto& line : g_app.terminal.lines) logs += line.text;
        return logs;
    };
    auto logs = execute("#include <cstdio>\nint main(){ std::puts(\"workflow-ok\"); return 0; }\n");
    CHECK(logs.find("workflow-ok") != std::string::npos);
    std::string buildLog, terminalLog;
    for(const auto& line:g_app.console.lines) buildLog+=line.text;
    for(const auto& line:g_app.terminal.lines) terminalLog+=line.text;
    CHECK(buildLog.find("workflow-ok")==std::string::npos);
    CHECK(terminalLog.find("workflow-ok")!=std::string::npos);
    CHECK(g_app.showTerminal && g_app.focusTerminal);
    CHECK(logs.find(i18n::IsEnglish()?"exit code 0":"退出码 0") != std::string::npos);
    CHECK(!g_app.procBusy.load() && !g_app.runInteractive.load());
    CHECK(!fs::exists(dir / L"build_out.txt") && !fs::exists(dir / L"build_err.txt"));
    bool taskLogs=false;
    for(auto& task:execution::ListTasks())if(task.cwd==dir.wstring()&&task.state=="completed"){
        auto record=fs::path(task.task.directory);
        taskLogs=fs::is_regular_file(record/"stdout.bin")&&fs::is_regular_file(record/"stderr.bin");
        if(taskLogs)break;
    }
    CHECK(taskLogs);

    const auto firstArtifact=g_app.lastArtifacts.at(source.wstring());
    CHECK(fs::exists(firstArtifact));
    utils::WriteFileU8((dir/L"main.exe").wstring(),"old-exe-sentinel");
    logs=execute("#include <cstdio>\nint main(){ std::puts(\"compile-only-marker\"); }\n",BuildAction::Compile);
    CHECK(logs.find("compile-only-marker")==std::string::npos);
    const auto secondArtifact=g_app.lastArtifacts.at(source.wstring());
    CHECK(firstArtifact!=secondArtifact && fs::exists(firstArtifact)&&fs::exists(secondArtifact));
    std::string sentinel; CHECK(utils::ReadFileU8((dir/L"main.exe").wstring(),sentinel)); CHECK_EQ(sentinel,std::string("old-exe-sentinel"));
    logs=execute("uncompiled invalid edit",BuildAction::Run);
    CHECK(logs.find("compile-only-marker")!=std::string::npos);
    CHECK_EQ(g_app.lastArtifacts.at(source.wstring()),secondArtifact);
    logs = execute("int main(){ return missing_name; }\n");
    CHECK(logs.find("失败") != std::string::npos);
    CHECK(!g_app.activeDiags.empty()); // Manual build diagnostics survive autoCheck=false.
    const auto fixture = fs::path(utils::GetExeDir()) / L"execution_fixture.exe";
    CHECK(fs::exists(fixture));
    if (fs::exists(fixture)) {
        g_app.settings.useCmdTemplate = true;
        g_app.settings.commandTemplate = "\"" + utils::WToU8(fixture.wstring()) + "\" sleep";
        g_app.StartCompileRun();
        Sleep(150); const auto start = GetTickCount64();
        g_app.killRequested.store(true);
        if (g_app.procThread.joinable()) g_app.procThread.join();
        CHECK(GetTickCount64() - start < 1500);
        g_app.PumpProcResults();
        CHECK(!g_app.runInteractive.load() && !g_app.procBusy.load());
    }
    g_app.editors = std::move(savedEditors); g_app.active = savedActive;
    g_app.settings = savedSettings; g_app.dataDir = savedDir;
    g_app.lastArtifacts=savedArtifacts;
    g_app.activeDiags.clear(); g_app.diagResults.clear(); g_app.diagDoneRev = 0;
    g_app.diagPending = false; g_app.killRequested.store(false);
    std::error_code ec;
    auto expectedParent = fs::temp_directory_path();
    if (expectedParent.filename().empty()) expectedParent = expectedParent.parent_path();
    if (dir.parent_path() == expectedParent && dir.filename().wstring().find(L"MiniCPP workflow ") == 0)
        fs::remove_all(dir, ec);
    CHECK(!ec);
}

// 符号索引: 类内成员/签名提取、typedef 别名解析、库内部实现细节过滤、系统头跟随
static void TestSymbolIndex() {
    static const std::string doc =
        "class Widget {\n"
        "public:\n"
        "    int size() const;\n"
        "    void push_back(int x);\n"
        "    int count;\n"
        "    void _M_impl();\n"
        "    int __secret;\n"
        "};\n"
        "typedef Widget MyWidget;\n";
    std::vector<DocRef> docs = { { 999999, &doc } };
    auto ms = SymbolIndex::I().MembersOfType("MyWidget", docs);
    bool hasSize = false, hasPush = false, hasCount = false, hasNoise = false;
    std::string sizeSig, pushSig;
    for (auto& m : ms) {
        if (m.name == "size") { hasSize = true; sizeSig = m.sig; }
        if (m.name == "push_back") { hasPush = true; pushSig = m.sig; }
        if (m.name == "count") hasCount = true;
        if (m.name == "_M_impl" || m.name == "__secret") hasNoise = true;
    }
    CHECK(hasSize); CHECK(hasPush); CHECK(hasCount);      // typedef 别名可解析到类成员
    CHECK(!hasNoise);                                     // _M_/__ 内部实现细节被过滤
    CHECK_EQ(sizeSig, std::string("size()"));             // 提取到真实签名
    CHECK_EQ(pushSig, std::string("push_back(int x)"));

    // 系统头跟随: <vector> 的真实定义在 <bits/stl_vector.h>, 需跟进去才能拿到成员
    // (依赖本机 g++ include 路径; 无编译器时跳过, 不算失败)
    const auto& dirs = SymbolIndex::I().IncludeDirs();
    if (dirs.empty()) {
        std::printf("  (跳过系统头测试: 未解析到 include 路径)\n");
    } else {
        std::vector<DocRef> none;
        auto vs = SymbolIndex::I().MembersOfType("vector", none);
        bool hasPushBack = false;
        for (auto& m : vs) if (m.name == "push_back") hasPushBack = true;
        CHECK(hasPushBack);
    }
}

static void TestCommentLinks() {
    auto links = FindCommentLinks(u8"参考：https://example.com/docs?q=1&lang=zh#part，另见 http://example.org/a。");
    CHECK_EQ(links.size(), (size_t)2);
    if (links.size() == 2) {
        CHECK_EQ(links[0].url, "https://example.com/docs?q=1&lang=zh#part");
        CHECK_EQ(links[1].url, "http://example.org/a");
    }
    links = FindCommentLinks("(https://example.com/wiki/Foo_(bar)). <HTTP://example.org> www.example.com!");
    CHECK_EQ(links.size(), (size_t)3);
    if (links.size() == 3) {
        CHECK_EQ(links[0].url, "https://example.com/wiki/Foo_(bar)");
        CHECK_EQ(links[1].url, "HTTP://example.org");
        CHECK_EQ(links[2].url, "https://www.example.com");
    }
    CHECK(FindCommentLinks("javascript:alert(1) file:///C:/test ftp://example.com").empty());
    CHECK(FindCommentLinks("https:// http:// www.").empty());
    CHECK(FindCommentLinks("abchttps://example.com").empty());
    std::string text = "[doc](https://example.com/a#anchor)";
    links = FindCommentLinks(text);
    CHECK_EQ(links.size(), (size_t)1);
    if (!links.empty()) CHECK_EQ(text.substr(links[0].begin, links[0].end - links[0].begin), links[0].url);
}

static void TestCompletionDocumentation() {
    ImGui::CreateContext(); LoadDocumentationTestPlugin();
    auto doc = [](const std::string& s) { return CompletionDocBefore(s, s.find("int value")); };
    CHECK_EQ(doc("// first\n/// second\nint value;"), "first\nsecond");
    CHECK_EQ(doc("/** brief\n * details\n */\nint value;"), "brief\ndetails");
    CHECK_EQ(doc("/* one line */\r\nint value;"), "one line");
    CHECK_EQ(doc("// unrelated\n\nint value;"), "");
    CHECK_EQ(doc("// unrelated\n#define OTHER 1\nint value;"), "");
    CHECK_EQ(doc("int other; /* trailing */\nint value;"), "");
    CHECK_EQ(doc("// first\n\n// actual\nint value;"), "actual");
    CHECK_EQ(doc("*pointer;\nint value;"), "");
    CHECK_EQ(CompletionDocBefore("abc", 99), "");
    CHECK_EQ(CompletionDocBefore("int value; /**< inline description */",0),"inline description");
    std::string grouped="/** @name Streams\n * shared paragraph\n *\n * final paragraph https://example.com/io\n */\n///@{\nint cin; /// input stream\nint cout; /// output stream\n///@}\nint outside;";
    auto inputDoc=CompletionDocBefore(grouped,grouped.find("int cin"));
    auto outputDoc=CompletionDocBefore(grouped,grouped.find("int cout"));
    CHECK(inputDoc.find("shared paragraph")!=std::string::npos);
    CHECK(inputDoc.find("final paragraph")!=std::string::npos);
    CHECK(inputDoc.find("input stream")!=std::string::npos);
    CHECK(inputDoc.find("@{")==std::string::npos);
    CHECK(outputDoc.find("shared paragraph")!=std::string::npos);
    CHECK(outputDoc.find("output stream")!=std::string::npos);
    CHECK(CompletionDocBefore(grouped,grouped.find("int outside")).empty());
    std::string longComment="/**\n";
    for(int i=0;i<180;++i) longComment+=" * paragraph "+std::to_string(i)+"\n";
    longComment+=" */\nint value;";
    CHECK(doc(longComment).find("paragraph 179")!=std::string::npos);
    CHECK(doc(longComment).find("paragraph 0")!=std::string::npos);


    auto candidate = [](CodeEditor& ed, const std::string& name) {
        ed.RebuildLines(); ed.m_cursor = ed.m_text.size(); ed.OpenCompletion();
        for (auto& item : ed.m_compItems) if (item.label == name) return item;
        CHECK(false);
        return CodeEditor::CompItem{};
    };
    CodeEditor local;
    local.SetText("void f() {\n// local description\nint localCount;\nloc");
    auto it = candidate(local, "localCount");
    CHECK_EQ(it.hint, "int"); CHECK_EQ(it.doc, "local description");
    CodeEditor param;
    param.SetText("void f(double parameterValue) { par");
    CHECK_EQ(candidate(param, "parameterValue").hint, "double");
    const std::string cls = "struct Base {\n// member description\nint memberCount;\n"
                            "enum Mode {\n// enum description\nreadyFlag\n};\n};\n"
                            "struct Widget : Base {};\n";
    CodeEditor member;
    member.SetText(cls + "void f(){ Widget obj; obj.mem");
    it = candidate(member, "memberCount");
    CHECK_EQ(it.doc, "member description"); CHECK_EQ(it.hint, "int");
    member.SetText(cls + "void f(){ Widget* obj; obj->mem");
    CHECK_EQ(candidate(member, "memberCount").doc, "member description");
    member.SetText(cls + "Widget::rea");
    it = candidate(member, "readyFlag");
    CHECK_EQ(it.doc, "enum description"); CHECK_EQ(it.hint, "Mode");
    CodeEditor ns;
    ns.SetText("namespace sample {\n// namespace description\nlong countValue;\n}\nsample::cou");
    it = candidate(ns, "countValue");
    CHECK_EQ(it.doc, "namespace description"); CHECK_EQ(it.hint, "long");
    auto other = std::make_unique<CodeEditor>();
    other->SetText("// other tab description\nfloat crossTabValue;\n");
    g_app.editors.push_back(std::move(other));
    CodeEditor cross;
    cross.SetText("cro");
    it = candidate(cross, "crossTabValue");
    CHECK_EQ(it.doc, "other tab description"); CHECK_EQ(it.hint, "float");
    g_app.editors.pop_back();
    member.SetText(cls + "rea");
    CHECK_EQ(candidate(member, "readyFlag").hint, "Mode");
    CodeModel model;
    std::string declarations = "class Actual; typedef Actual Alias; "
        "template<class T = decltype(1 << 2)> class Actual { public: int value; };";
    model.Rebuild(declarations);
    size_t actualPos = 0, aliasPos = 0;
    CHECK(model.FindDeclPos("Actual", declarations.size(), actualPos));
    CHECK(model.FindDeclPos("Alias", declarations.size(), aliasPos));
    CHECK_EQ(actualPos, aliasPos);
    CHECK(actualPos > declarations.find("template"));
    std::vector<CMember> members;
    CHECK(model.CollectMembers("Actual", members));
    CHECK(!members.empty());
    CodeEditor fn;
    std::string signature = "long descriptiveFunctionName(int firstArgument, double secondArgument, const char* thirdArgument)";
    fn.SetText("// function description\n" + signature + ";\ndesc");
    it = candidate(fn, "descriptiveFunctionName");
    CHECK(it.hint.size() > 64); CHECK_EQ(it.doc, "function description");

    if (!SymbolIndex::I().IncludeDirs().empty()) {
        // A real nested include verifies offsets into the expanded header source.
        auto dir = std::filesystem::temp_directory_path() /
                   ("minicpp_completion_" + std::to_string(GetCurrentProcessId()));
        std::filesystem::create_directories(dir);
        auto inner = dir / "inner.h", outer = dir / "outer.h";
        CHECK(utils::WriteFileU8(inner.wstring(),
            "// inner namespace documentation\nnamespace fixture {\n// header variable description\nint headerCount;\n}\n"
            "struct HeaderClass {\n// header member description\nint headerMember;\n};\n"));
        CHECK(utils::WriteFileU8(outer.wstring(), "#include \"inner.h\"\n// outer namespace documentation\nnamespace fixture {}\n"));
        CodeEditor hdr;
        hdr.SetPath((dir / "main.cpp").wstring());
        hdr.SetText("#include \"outer.h\"\nfixture::hea");
        it = candidate(hdr, "headerCount");
        CHECK_EQ(it.doc, "header variable description"); CHECK_EQ(it.hint, "int");
        CHECK(!it.sources.empty() && it.sources.front().file==inner.wstring());
        hdr.SetText("#include \"outer.h\"\nfix");
        auto namespaceItem=candidate(hdr,"fixture");
        CHECK(namespaceItem.sources.size()==2);
        bool innerDoc=false,outerDoc=false;
        for(const auto& source:namespaceItem.sources) {
            if(source.file==inner.wstring() && source.doc=="inner namespace documentation") innerDoc=true;
            if(source.file==outer.wstring() && source.doc=="outer namespace documentation") outerDoc=true;
        }
        CHECK(innerDoc && outerDoc);
        hdr.SetText("#include \"outer.h\"\nusing namespace fixture;\nhea");
        CHECK_EQ(candidate(hdr, "headerCount").doc, "header variable description");
        hdr.SetText("#include \"outer.h\"\nHeaderClass::hea");
        CHECK_EQ(candidate(hdr, "headerMember").doc, "header member description");
        hdr.SetText("#include \"outer.h\"\nvoid f(){ HeaderClass obj; obj.hea");
        CHECK_EQ(candidate(hdr, "headerMember").doc, "header member description");
        std::filesystem::remove(inner); std::filesystem::remove(outer); std::filesystem::remove(dir);
        hdr.SetText("#include <iostream>\nstd::ci");
        auto inputItem=candidate(hdr,"cin");
        CHECK(!inputItem.hint.empty());
        CHECK(inputItem.doc.find("Linked to standard input")!=std::string::npos);
        CHECK(inputItem.doc.find("startup and termination")!=std::string::npos);
        CHECK(inputItem.doc.find("@{")==std::string::npos);
        CHECK(!inputItem.sources.empty() && std::filesystem::path(inputItem.sources.front().file).filename()==L"iostream");
        hdr.SetText("#include <iostream>\nstd::ios::goo");
        CHECK_EQ(candidate(hdr, "goodbit").label, "goodbit");
    }
    UnloadDocumentationTestPlugin(); ImGui::DestroyContext();
}

static void TestEditorDecorationsAndColors() {
    CodeEditor ed;
    ed.SetText("int globalCount;\nvoid sample(int argument) {\n\tint localCount=0;\n\n"
        "    const char* s=\"ignore ([{\"; // ignore })]\n    if(localCount){ localCount++; }\n}\n");
    ed.RebuildLines(); ed.RebuildCommentCache(); ed.EnsureCodeModel(); ed.RebuildDecorations();
    CHECK_EQ(ed.m_indentColumns[2], 4); CHECK_EQ(ed.m_indentColumns[3], 4);
    size_t literal=ed.m_text.find("([{");
    CHECK_EQ(ed.m_bracketLevel[literal], -1);
    size_t brace=ed.m_text.find('{');
    CHECK(ed.m_bracketLevel[brace] >= 0);
    CHECK_EQ(ed.m_bracketLevel[brace], ed.m_bracketLevel[ed.m_text.rfind('}')]);
    ed.SetText("int 数量 = 3;"); ed.RebuildLines();
    bool unicodeComment=false; auto unicodeTokens=ed.TokenizeLine(0,unicodeComment);
    bool wholeUnicode=false;
    for(const auto& token:unicodeTokens) if(ed.m_text.substr(token.start,token.len)=="数量") wholeUnicode=true;
    CHECK(wholeUnicode);
    bool block=false;
    ed.SetText("/* comment\nend */ int value;"); ed.RebuildLines();
    ed.TokenizeLine(0,block); CHECK(block);
    auto tokens=ed.TokenizeLine(1,block); CHECK(!block);
    CHECK(!tokens.empty() && tokens.front().type==TokenKind::Comment);
    bool hasCode=false; for(auto& token:tokens) if(token.type==TokenKind::Keyword) hasCode=true;
    CHECK(hasCode);

    auto savedSettings=g_app.settings;
    ImGui::CreateContext();
    g_app.settings.theme="dark";
    g_app.settings.editorColors["dark/Selection"]=IM_COL32(35,80,140,255);
    g_app.settings.editorColors["dark/SelectedText"]=IM_COL32(255,240,210,255);
    g_app.ApplyTheme();
    CHECK_EQ(g_app.colors.selection, IM_COL32(35,80,140,255));
    CHECK_EQ(g_app.EditorColorValue(EditorColor::SelectedText), IM_COL32(255,240,210,255));
    g_app.settings.theme="light"; g_app.ApplyTheme();
    CHECK(g_app.colors.selection != IM_COL32(35,80,140,255));
    g_app.settings=savedSettings; g_app.ApplyTheme();
    ImGui::DestroyContext();
}

static void TestNamespaceAndExactCompletion() {
    CodeModel model;
    model.Rebuild("namespace workspace {} namespace outer::inner { int value; } namespace workspace { int item; }");
    int workspaces = 0;
    for (auto& symbol : model.AllSymbols()) if (symbol.name == "workspace") {
        ++workspaces; CHECK(symbol.kind == CSymKind::Namespace);
    }
    CHECK_EQ(workspaces, 1);
    CHECK(model.FindType("workspace") == nullptr);
    CHECK(model.FindType("outer") == nullptr);
    std::vector<CSym> nested;
    model.NamespaceSymbols("outer", false, nested);
    bool hasInner = false;
    for (auto& symbol : nested) if (symbol.name == "inner" && symbol.kind == CSymKind::Namespace) hasInner = true;
    CHECK(hasInner);

    auto visibleAt=[&](const std::string& source,const std::string& name) {
        model.Rebuild(source); std::vector<CSym> visible;
        model.VisibleSymbols(source.size(),visible);
        for(const auto& symbol:visible) if(symbol.name==name) return true;
        return false;
    };
    CHECK(!visibleAt("namespace streams { int source; } void f(){ sou", "source"));
    CHECK(visibleAt("namespace streams { int source; } using namespace streams; void f(){ sou", "source"));
    CHECK(visibleAt("namespace streams { int source; } void f(){ using streams::source; sou", "source"));
    CHECK(!visibleAt("namespace streams { int source; } void f(){ { using namespace streams; } sou", "source"));
    CHECK(!visibleAt("namespace streams { int source; } void f(){ using namespace streams; } void g(){ sou", "source"));
    CHECK(visibleAt("namespace streams { int source; void f(){ sou", "source"));
    CHECK(!visibleAt("namespace streams { int source; } namespace other { void f(){ sou", "source"));
    CHECK(visibleAt("namespace streams { int source; } namespace bridge { using namespace streams; } using namespace bridge; void f(){ sou", "source"));
    CHECK(visibleAt("namespace outer { namespace inner { int source; } using namespace inner; void f(){ sou", "source"));
    CHECK(visibleAt("namespace outer { inline namespace version { int source; } } using namespace outer; void f(){ sou", "source"));

    CodeEditor ed;
    auto find = [&](const std::string& source, const std::string& name) {
        ed.SetText(source); ed.RebuildLines(); ed.m_cursor = ed.m_text.size(); ed.OpenCompletion();
        for (const auto& item : ed.m_compItems) if (item.label == name) return item;
        printf("Missing exact/namespace completion: %s\n", name.c_str()); CHECK(false);
        return CodeEditor::CompItem{};
    };
    CHECK_EQ(std::string(find("namespace workspace{} void f(){ work", "workspace").kind), "命名空间");
    CHECK_EQ(std::string(find("namespace workspace{} void f(){ workspace", "workspace").kind), "命名空间");
    CHECK_EQ(std::string(find("struct Widget{}; void f(){ Widget", "Widget").kind), "类型");
    CHECK_EQ(find("void f(){ int countValue; countValue", "countValue").hint, "int");
    CHECK(find("int functionName(int value); void f(){ functionName", "functionName").func);
    if (SymbolIndex::I().IncludeDirs().empty()) return;
    for (const char* prefix : { "st", "std", "STD" }) {
        auto item = find(std::string("#include <iostream>\nvoid f(){ ") + prefix, "std");
        CHECK_EQ(std::string(item.kind), "命名空间"); CHECK(!item.func);
        CHECK(ed.m_hdr.FindType("std") == nullptr);
        for (const auto& symbol : ed.m_hdr.AllSymbols()) if (symbol.name == "std") CHECK(symbol.kind == CSymKind::Namespace);
    }
    auto hasUnqualified=[&](const std::string& body,const char* name) {
        ed.SetText("#include <iostream>\n"+body); ed.RebuildLines();
        ed.m_cursor=ed.m_text.size(); ed.OpenCompletion();
        for(const auto& item:ed.m_compItems) if(item.label==name) return true;
        return false;
    };
    CHECK(!hasUnqualified("void f(){ cin", "cin"));
    CHECK(!hasUnqualified("void f(){ wcin", "wcin"));
    CHECK(hasUnqualified("using namespace std; void f(){ cin", "cin"));
    CHECK(hasUnqualified("void f(){ using namespace std; cin", "cin"));
    CHECK(hasUnqualified("void f(){ using std::cin; cin", "cin"));
    CHECK(!hasUnqualified("void f(){ using std::cout; cin", "cin"));
    CHECK(!hasUnqualified("void f(){ { using namespace std; } cin", "cin"));
    CHECK(!hasUnqualified("void f(){ using namespace std; } void g(){ cin", "cin"));
    CHECK(!hasUnqualified("// using namespace std;\nvoid f(){ cin", "cin"));
    CHECK(!hasUnqualified("void f(){ const char* s=\"using namespace std;\"; cin", "cin"));
    CHECK(hasUnqualified("void f(){ int cin=0; cin", "cin"));
    CHECK(hasUnqualified("void f(){ std::cin", "cin"));
    find("#include <iostream>\nvoid f(){ st", "std");
    ed.InsertText("d"); ed.UpdateCompletion();
    bool keptExactStd = false;
    for (const auto& item : ed.m_compItems)
        if (item.label == "std" && std::string(item.kind) == "命名空间") keptExactStd = true;
    CHECK(ed.m_compOpen); CHECK(keptExactStd);
    ed.SetText("#include <iostream>\nvoid f(){ stdx");
    ed.RebuildLines(); ed.m_cursor = ed.m_text.size(); ed.OpenCompletion();
    bool hasStd = false;
    for (const auto& item : ed.m_compItems) if (item.label == "std") hasStd = true;
    CHECK(!hasStd);
    ed.SetText("#include <iostream>\nvoid f(){ std::");
    ed.RebuildLines(); ed.m_cursor = ed.m_text.size(); ed.OpenCompletion();
    bool hasCin = false, hasSelfNamespace = false;
    for (const auto& item : ed.m_compItems) {
        if (item.label == "cin") hasCin = true;
        if (item.label == "std") hasSelfNamespace = true;
    }
    CHECK(hasCin); CHECK(!hasSelfNamespace);
}

static void TestQualifiedStreamCompletion() {
    CodeModel model;
    std::string code = "struct Input { int read(); }; struct Other { int unrelated; }; "
        "namespace streams { Input source; Input make(); namespace nested { Input source; } } "
        "namespace other { Other source; } void f(){ Other source; }";
    model.Rebuild(code);
    CHECK_EQ(model.EvalExprType("streams::source", code.size() - 2), "Input");
    CHECK_EQ(model.EvalExprType("streams::make()", code.size() - 2), "Input");
    CHECK_EQ(model.EvalExprType("streams::nested::source", code.size() - 2), "Input");
    CHECK_EQ(model.EvalExprType("other::source", code.size() - 2), "Other");
    CHECK(model.EvalExprType("missing::source", code.size()).empty());

    CodeModel header;
    header.SetSystemSource(true);
    header.Rebuild("template<class A, class B> struct Output { void flush(); }; "
        "struct Input { typedef Input __input_type; __input_type& getline(char* data, int size); "
        "Output<char, int>* tie() const; }; namespace std { Input cin; }");
    std::vector<CMember> parsed;
    CHECK(header.CollectMembers("Input", parsed));
    bool hasGetline = false, hasTie = false;
    for (const auto& member : parsed) {
        if (member.name == "getline") hasGetline = true;
        if (member.name == "tie") hasTie = true;
    }
    CHECK(hasGetline); CHECK(hasTie);
    CHECK_EQ(header.EvalExprType("std::cin.tie()", 9999), "Output");
    if (SymbolIndex::I().IncludeDirs().empty()) return;
    CodeEditor ed;
    auto expect = [&](const std::string& expression, std::initializer_list<const char*> names) {
        ed.SetText("#include <iostream>\nvoid f(){\n" + expression);
        ed.RebuildLines(); ed.m_cursor = ed.m_text.size(); ed.OpenCompletion();
        CHECK(ed.m_compOpen);
        for (auto name : names) {
            bool found = false;
            for (const auto& item : ed.m_compItems) if (item.label == name) {
                found = true; CHECK(item.func); CHECK(!item.hint.empty()); break;
            }
            if (!found) printf("Missing stream member: %s -> %s\n", expression.c_str(), name);
            CHECK(found);
        }
    };
    expect("std::cin.", {"get", "getline", "ignore", "read", "good", "clear"});
    expect("std::cin.getl", {"getline"});
    expect("std::cout.", {"put", "write", "flush", "good"});
    expect("std::cerr.fl", {"flush"});
    expect("std::clog.fl", {"flush"});
    expect("using namespace std; cin.", {"get", "getline"});
    expect("auto& input = std::cin; input.", {"get", "getline"});
    expect("std::cin.tie()->", {"flush", "put"});
    ed.SetText("#include <iostream>\nvoid f(){ cin.");
    ed.RebuildLines(); ed.m_cursor=ed.m_text.size(); ed.OpenCompletion();
    CHECK(ed.m_compItems.empty());
    ed.SetText("void f(){ std::cin.");
    ed.RebuildLines(); ed.m_cursor = ed.m_text.size(); ed.OpenCompletion();
    CHECK(ed.m_compItems.empty()); // No included declaration means no invented library members.
}

static void TestCompletionPopupLayout() {
    ImGui::CreateContext(); LoadDocumentationTestPlugin();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.Fonts->AddFontDefault();
    unsigned char* pixels; int w, h;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
    io.DeltaTime = 1.0f / 60.0f;
    CodeEditor ed;
    ed.SetText("int x;\n\tint tabbed;\n");
    for (ImVec2 size : { ImVec2(1280, 840), ImVec2(480, 320) }) {
        io.DisplaySize = size;
        for (int frame = 0; frame < 3; ++frame) {
            ImGui::NewFrame();
            ImGui::SetNextWindowPos(ImVec2(size.x - 240, size.y - 200));
            ImGui::SetNextWindowSize(ImVec2(230, 190));
            ImGui::Begin("test_editor", nullptr, ImGuiWindowFlags_NoSavedSettings);
            ed.m_cursor = ed.m_text.size();
            ed.m_compOpen = true; ed.m_compIdx = 0;
            ed.m_compItems.clear();
            CodeEditor::CompItem item{ "candidate", "variable", "int", false };
            for (int n = 0; n < 60; ++n) item.doc += "Documentation https://example.com/very/long/path?parameter=value#anchor and more text\n";
            item.sources.push_back({L"fixture.h",1,item.doc});
            for (int n = 0; n < 20; ++n) ed.m_compItems.push_back(item);
            ed.Draw();
            CHECK(ed.CharW() > 1 && std::isfinite(ed.CharW()));
            CHECK(std::abs(ed.CharX(ed.m_text.find("int tabbed")) - ed.CharW()*4) < 0.1f);
            ImGui::End();
            ImGui::Render();
            auto* popup = ImGui::FindWindowByName("##completion");
            CHECK(popup != nullptr);
            if (popup) {
                CHECK(popup->Pos.x >= 0 && popup->Pos.y >= 0);
                CHECK(popup->Pos.x + popup->Size.x <= size.x + 0.1f);
                CHECK(popup->Pos.y + popup->Size.y <= size.y + 0.1f);
                bool scrollableDocs = false;
                for (auto* win : ImGui::GetCurrentContext()->Windows)
                    if (win->ParentWindow == popup && win->ScrollMax.y > 0) scrollableDocs = true;
                if (frame > 0) CHECK(scrollableDocs);
            }
        }
    }
    if (!SymbolIndex::I().IncludeDirs().empty()) {
        CodeEditor jump;
        jump.SetText("#include <iostream>\n");
        jump.RebuildLines();
        jump.m_cursor = jump.m_text.find("iostream") + 2;
        size_t count = g_app.editors.size();
        int oldActive = g_app.active;
        CHECK(jump.GotoDefinition());
        CHECK_EQ(g_app.editors.size(), count + 1);
        if (g_app.editors.size() > count) {
            CHECK_EQ(std::filesystem::path(g_app.editors.back()->m_path).filename().string(), "iostream");
            g_app.editors.pop_back();
        }
        g_app.active = oldActive;
    }
    UnloadDocumentationTestPlugin(); ImGui::DestroyContext();
}


static void TestInternationalizationAndCommunity(){
    CHECK_EQ(std::string(i18n::ResolveLanguage("auto",MAKELANGID(LANG_ENGLISH,SUBLANG_ENGLISH_US))),std::string("en"));
    CHECK_EQ(std::string(i18n::ResolveLanguage("auto",MAKELANGID(LANG_CHINESE,SUBLANG_CHINESE_SIMPLIFIED))),std::string("zh-CN"));
    CHECK_EQ(std::string(i18n::ResolveLanguage("zh-CN",MAKELANGID(LANG_ENGLISH,SUBLANG_ENGLISH_US))),std::string("zh-CN"));
    i18n::SetLanguage("en");CHECK_EQ(std::string(i18n::Tr("设置")),std::string("Settings"));
    CHECK(ImHashStr(i18n::Window("设置"))==ImHashStr("设置"));
    CHECK(ImHashStr(i18n::Window("Settings"))==ImHashStr("设置"));
    CHECK_EQ(std::string(i18n::Tr("untouched.cpp")),std::string("untouched.cpp"));
    const char* combo=i18n::Tr("紧凑\0标准\0宽松\0");CHECK_EQ(std::string(combo),std::string("Compact"));
    CHECK_EQ(std::string(combo+strlen(combo)+1),std::string("Standard"));
    Settings setting;CHECK_EQ(setting.language,std::string("auto"));setting.language="invalid";setting.Normalize();CHECK_EQ(setting.language,std::string("auto"));
    auto file=std::filesystem::temp_directory_path()/(L"ccride-language-"+std::to_wstring(GetCurrentProcessId())+L".json");
    std::string error;setting.language="en";CHECK(setting.Save(file.wstring()));Settings loaded;CHECK(loaded.Load(file.wstring()));CHECK_EQ(loaded.language,std::string("en"));
    CHECK(setting.ExportPreferences(file.wstring(),error));loaded.language="zh-CN";CHECK(loaded.ImportPreferences(file.wstring(),error));CHECK_EQ(loaded.language,std::string("en"));std::filesystem::remove(file);
    CHECK_EQ(community::ParseConfig("{\"schema\":1}").repository,std::string("https://github.com/yunyugong/ccrIDE"));
    auto rejects=[](const std::function<void()>& f){try{f();return false;}catch(...){return true;}};
    CHECK(rejects([](){community::ParseConfig("{\"schema\":2}");}));
    CHECK(rejects([](){community::ParseConfig("{\"schema\":1,\"issues\":\"file:///C:/bad\"}");}));
    CHECK(rejects([](){community::ParseConfig("{\"schema\":1,\"issues\":\"https://user@github.com/x\"}");}));
    CHECK(community::DownloadUrl("http://127.0.0.1:1234/package.zip"));CHECK(!community::DownloadUrl("http://example.com/package.zip"));
    CHECK(!community::DownloadUrl("http://localhost:1234@evil.example/x"));
    nlohmann::json entry={{"id","probe"},{"name","Probe"},{"version","1.0"},{"entry","probe.dll"},{"apiVersion",2},{"packageUrl","https://github.com/example/repo/releases/download/v1/probe.zip"},{"sha256",std::string(64,'a')}};
    auto catalog=[&](const nlohmann::json& item){return nlohmann::json{{"schema",1},{"plugins",nlohmann::json::array({item})}}.dump();};
    CHECK(community::ParseMarket(catalog(entry)).size()==1);
    CHECK_EQ(community::ParseMarket(catalog(entry))[0].packageUrl,entry["packageUrl"].get<std::string>());
    for(auto id:{"../escape","bad/name","x:ads",""}){auto invalid=entry;invalid["id"]=id;CHECK(rejects([&](){community::ParseMarket(catalog(invalid));}));}
    for(auto pair:{std::pair<const char*,const char*>{"sha256","bad"},{"packageUrl","http://example.com/bad.zip"},{"entry","../bad.dll"}}){auto invalid=entry;invalid[pair.first]=pair.second;CHECK(rejects([&](){community::ParseMarket(catalog(invalid));}));}
    auto invalid=entry;invalid["apiVersion"]=99;CHECK(rejects([&](){community::ParseMarket(catalog(invalid));}));
    invalid=entry;invalid["dllUrl"]="https://example.com/probe.dll";CHECK(rejects([&](){community::ParseMarket(catalog(invalid));}));
    CHECK(rejects([&](){community::ParseMarket(nlohmann::json{{"schema",1},{"plugins",nlohmann::json::array({entry,entry})}}.dump());}));
    i18n::SetLanguage("zh-CN");CHECK_EQ(std::string(i18n::Tr("设置")),std::string("设置"));
}

static int CheckPortablePackage() {
    if (!g_app.Init() || !g_app.portableMode) return 10;
    auto root = std::filesystem::path(g_app.executableDir);
    auto compiler = std::filesystem::path(utils::U8ToW(g_app.settings.gppPath));
    CHECK(compiler == root / L"toolchain" / L"mingw64" / L"bin" / L"g++.exe");
    CHECK(std::filesystem::path(g_app.dataDir) == root / L"data");
    CHECK(std::filesystem::path(g_app.pluginDir) == root / L"plugins");
    Settings original = g_app.settings;
    original.Save(g_app.settingsPath, g_app.executableDir);
    Settings stored; CHECK(stored.Load(g_app.settingsPath));
    CHECK_EQ(stored.gppPath, std::string("toolchain/mingw64/bin/g++.exe"));
    auto source = std::filesystem::path(g_app.dataDir) / L"package-check.cpp";
    utils::WriteFileU8(source.wstring(), "#include <iostream>\n#include <vector>\nint main(){std::vector<int> values{1,2,3}; std::cout << \"portable-package-ok \" << values.size();}\n");
    g_app.NewFile(source.wstring());
    g_app.settings.autoCheck = false; g_app.settings.useCmdTemplate = false;
    g_app.settings.runArguments.clear(); g_app.settings.runDirectory.clear(); g_app.settings.runEnvironment.clear();
    g_app.StartCompileRun();
    if (g_app.procThread.joinable()) g_app.procThread.join();
    g_app.PumpProcResults();
    std::string logs; for (const auto& line : g_app.console.lines) logs += line.text;
        for (const auto& line : g_app.terminal.lines) logs += line.text;
    CHECK(logs.find("portable-package-ok 3") != std::string::npos);
    CHECK(logs.find(i18n::IsEnglish()?"exit code 0":"退出码 0") != std::string::npos);
    bool vectorHeader=false;
    for(auto& dir:SymbolIndex::I().IncludeDirs())if(std::filesystem::is_regular_file(std::filesystem::path(dir)/L"vector"))vectorHeader=true;
    CHECK(vectorHeader);
    g_app.StartCompileRun(BuildAction::Debug);
    if(g_app.procThread.joinable()) g_app.procThread.join();
    auto debugStart=GetTickCount64();
    while(g_app.debugger.Snapshot().active&&!g_app.debugger.Snapshot().stopped&&GetTickCount64()-debugStart<7000) Sleep(20);
    auto debug=g_app.debugger.Snapshot();
    CHECK(debug.active&&debug.stopped);
    CHECK(debug.line>0 && debug.file==utils::WToU8(source.wstring()));
    if(!debug.stopped) std::printf("%s\n%s\n",debug.status.c_str(),debug.log.c_str());
    g_app.debugger.Stop(); g_app.debugger.Join();
    CHECK(!g_app.debugger.Snapshot().active);
    original.Save(g_app.settingsPath, g_app.executableDir);
    std::printf("Portable package: %d passed, %d failed\n", g_pass, g_fail);
    if (g_fail) std::printf("%s\n", logs.c_str());
    g_app.Shutdown();
    return g_fail ? 1 : 0;
}

static std::string languageFile,languageText,languageRequest;
static unsigned long long languageRevision=0;static size_t languageOffset=0;
static void LanguageDocument(const char* file,const char* text,size_t size,unsigned long long revision){languageFile=file;languageText.assign(text,size);languageRevision=revision;}
static void LanguageClose(const char*){}
static void LanguageRequest(const char* kind,const char*,unsigned long long revision,size_t offset){languageRequest=kind;languageRevision=revision;languageOffset=offset;}
static void TestPluginBoundaries(){
 namespace fs=std::filesystem;auto root=fs::temp_directory_path()/(L"ccride-plugin-boundary-"+std::to_wstring(GetCurrentProcessId()));fs::create_directories(root/"plugins/probe");fs::create_directories(root/"working");
 auto oldPlugin=g_app.pluginDir,oldData=g_app.dataDir;g_app.pluginDir=(root/"plugins").wstring();g_app.dataDir=(root/"data").wstring();fs::create_directories(g_app.dataDir);
 auto manifest=nlohmann::json{{"id","probe"},{"name","Dependency probe"},{"kind","soft"},{"entry","probe.dll"},{"apiVersion",2}};
 utils::WriteFileU8((root/"plugins/probe/plugin.json").wstring(),manifest.dump());
 fs::copy_file(fs::u8path(CCRIDE_FIXTURE_CONSUMER),root/"plugins/probe/probe.dll");
 fs::copy_file(fs::u8path(CCRIDE_FIXTURE_GOOD),root/"plugins/probe/ccride_fixture_dependency.dll");
 fs::copy_file(fs::u8path(CCRIDE_FIXTURE_BAD),root/"working/ccride_fixture_dependency.dll");
 auto cwd=fs::current_path();fs::current_path(root/"working");ImGui::CreateContext();
 g_app.console.Clear();g_app.plugins.ScanAndLoad();CHECK(g_app.plugins.plugins.size()==1&&g_app.plugins.plugins[0].loaded);
 bool correct=false;for(auto& line:g_app.console.lines)if(line.text.find("DEPENDENCY=1")!=std::string::npos)correct=true;CHECK(correct);g_app.plugins.UnloadAll();
 fs::remove(root/"plugins/probe/ccride_fixture_dependency.dll");g_app.plugins.ScanAndLoad();CHECK(g_app.plugins.plugins.size()==1&&!g_app.plugins.plugins[0].loaded);g_app.plugins.UnloadAll();
 fs::create_directories(root/"plugins/duplicate");auto duplicate=manifest;duplicate["id"]="PROBE";utils::WriteFileU8((root/"plugins/duplicate/plugin.json").wstring(),duplicate.dump());
 g_app.plugins.ScanAndLoad();CHECK(g_app.plugins.plugins.size()==2);for(auto& plugin:g_app.plugins.plugins)CHECK(!plugin.loaded&&plugin.loadError.find("相同 ID")!=std::string::npos);g_app.plugins.UnloadAll();
 fs::remove_all(root/"plugins/duplicate");fs::copy_file(fs::u8path(CCRIDE_FIXTURE_GOOD),root/"plugins/probe/ccride_fixture_dependency.dll");
 {std::fstream file(root/"plugins/probe/probe.dll",std::ios::in|std::ios::out|std::ios::binary);IMAGE_DOS_HEADER dos{};file.read(reinterpret_cast<char*>(&dos),sizeof(dos));file.seekp(dos.e_lfanew+sizeof(DWORD));WORD machine=IMAGE_FILE_MACHINE_I386;file.write(reinterpret_cast<char*>(&machine),sizeof(machine));}
 g_app.plugins.ScanAndLoad();CHECK(g_app.plugins.plugins.size()==1&&!g_app.plugins.plugins[0].loaded&&g_app.plugins.plugins[0].loadError.find("x64")!=std::string::npos);g_app.plugins.UnloadAll();
 for(const char* id:{"ai-assistant","clangd"}){
  manifest["id"]=id;
  utils::WriteFileU8((root/"plugins/probe/plugin.json").wstring(),manifest.dump());
  g_app.plugins.ScanAndLoad();
  CHECK(g_app.plugins.plugins.size()==1&&!g_app.plugins.plugins[0].loaded&&g_app.plugins.plugins[0].loadError.find("1.2.0")!=std::string::npos);
  g_app.plugins.UnloadAll();
 }
 ImGui::DestroyContext();fs::current_path(cwd);g_app.pluginDir=oldPlugin;g_app.dataDir=oldData;fs::remove_all(root);
}
static void TestAllTabRecovery(){
 namespace fs=std::filesystem;auto root=fs::temp_directory_path()/(L"ccride-alltabs-"+std::to_wstring(GetCurrentProcessId()));fs::create_directories(root);
 auto saved=std::move(g_app.editors);int active=g_app.active;auto dir=g_app.dataDir;auto oldSnapshot=std::move(g_app.recoverSnapshot);auto oldSelection=std::move(g_app.recoverSelection);bool pending=g_app.recoverPending;
 g_app.editors.clear();g_app.dataDir=root.wstring();g_app.active=-1;g_app.NewFile();g_app.NewFile();
 auto original=root/"original.cpp";utils::WriteFileU8(original.wstring(),"int original;");
 g_app.editors[0]->SetText("");g_app.editors[1]->SetText("int changed;");g_app.editors[1]->m_path=original.wstring();g_app.editors[1]->m_dirty=true;
 g_app.SaveRecovery();recovery::Snapshot snapshot;std::string error;CHECK(recovery::Load((root/"recovery").wstring(),snapshot,error));CHECK(snapshot.tabs.size()==2&&snapshot.tabs[0].text.empty()&&snapshot.tabs[1].text=="int changed;");
 g_app.recoverSnapshot=snapshot;g_app.recoverSelection={1,1};g_app.editors[1]->SetText("unrelated current");g_app.SaveRecovery();recovery::Snapshot unchanged;CHECK(recovery::Load((root/"recovery").wstring(),unchanged,error));CHECK(unchanged.tabs[1].text=="int changed;");
 utils::WriteFileU8(original.wstring(),"external change");g_app.editors.clear();g_app.active=-1;g_app.ApplyRecovery(true);CHECK(g_app.editors.size()==2&&g_app.editors[0]->m_text.empty()&&g_app.editors[1]->m_text=="int changed;");CHECK(g_app.editors[0]->m_path.empty()&&g_app.editors[1]->m_path.empty());std::string content;CHECK(utils::ReadFileU8(original.wstring(),content)&&content=="external change");
 g_app.editors=std::move(saved);g_app.active=active;g_app.dataDir=dir;g_app.recoverSnapshot=std::move(oldSnapshot);g_app.recoverSelection=std::move(oldSelection);g_app.recoverPending=pending;fs::remove_all(root);
}
static void TestMultiCaret(){
 CodeEditor ed;ed.SetText("abc\nabc\n");ed.RebuildLines();ed.m_cursor=ed.m_selAnchor=1;ed.AddCaret(5);ed.MultiReplace("X");CHECK_EQ(ed.m_text,std::string("aXbc\naXbc\n"));ed.Undo();CHECK_EQ(ed.m_text,std::string("abc\nabc\n"));ed.Redo();CHECK_EQ(ed.m_text,std::string("aXbc\naXbc\n"));
 ed.SetText("abc\nabc\n");ed.RebuildLines();ed.m_carets={{0,2},{4,6}};ed.MultiReplace("q\nz",0,true);CHECK_EQ(ed.m_text,std::string("qc\nzc\n"));ed.Undo();CHECK_EQ(ed.m_text,std::string("abc\nabc\n"));
 ed.SetText(u8"中x\n文y");ed.RebuildLines();ed.m_carets={{3,3},{8,8}};ed.MultiReplace("",-1);CHECK_EQ(ed.m_text,std::string("x\ny"));
}
static void TestLanguageHost() {
    auto saved=std::move(g_app.editors);int savedIndex=g_app.active;auto originalSettings=g_app.settings;
    g_app.editors.clear();g_app.editors.push_back(std::make_unique<CodeEditor>());g_app.active=0;
    g_app.settings.codeCompletion=true;g_app.settings.autoCheck=true;
    auto& ed=*g_app.editors.front();ed.m_path=(std::filesystem::temp_directory_path()/L"ccride-language-host.cpp").wstring();
    const std::string text="struct Box{int value;};\nvoid f(){Box box; box.va}";
    auto setup=[&](){ed.SetText(text);ed.RebuildLines();ed.m_cursor=ed.m_text.find("box.va")+6;ed.OpenCompletion();};
    PluginLanguageHooks hooks{LanguageDocument,LanguageClose,LanguageRequest};language::Register(&hooks);
    auto publish=[](nlohmann::json event){auto value=event.dump();language::Publish(value.data(),value.size());};
    publish({{"kind","status"},{"ready",true}});setup();
    CHECK(ed.m_languagePending && ed.m_compOpen && ed.m_compItems.empty());CHECK(languageRequest=="completion");
    auto completion=[&](){return nlohmann::json{{"kind","completion"},{"file",languageFile},{"revision",languageRevision},{"offset",languageOffset},{"items",nlohmann::json::array({{{"label","value"},{"text","value"},{"detail","int"},{"start",languageOffset-2},{"end",languageOffset}}})}};};
    language::RequestRename(ed,"renamed");CHECK(languageRequest=="rename:renamed");
    auto rename=[&](){return nlohmann::json{{"kind","rename"},{"file",languageFile},{"revision",languageRevision},{"offset",languageOffset},{"edits",nlohmann::json::array({{{"file",languageFile},{"range",{{"start",{{"line",0},{"character",15}}},{"end",{{"line",0},{"character",20}}}}},{"text","renamed"}}})}};};
    publish(rename());CHECK(language::ApplyRename());CHECK(ed.m_text.find("int renamed")!=std::string::npos);ed.Undo();CHECK(ed.m_text==text);
    setup();language::RequestRename(ed,"renamed");publish(rename());ed.m_text+=" ";CHECK(!language::ApplyRename());CHECK(ed.m_text.find("renamed")==std::string::npos);
    setup();
    auto stale=completion();ed.m_text.insert(0," ");publish(stale);CHECK(ed.m_compItems.empty());
    setup();auto current=completion();ed.CloseCompletion();publish(current);CHECK(!ed.m_compOpen);
    setup();publish(completion());CHECK(ed.m_compItems.size()==1);CHECK(ed.AcceptCompletion());CHECK(ed.m_text.find("box.value")!=std::string::npos);
    ed.Undo();CHECK(ed.m_text==text);
    setup();auto moved=completion();--ed.m_cursor;publish(moved);CHECK(ed.m_compItems.empty());
    setup();publish({{"kind","status"},{"ready",false}});
    CHECK(!language::Ready() && !ed.m_languagePending);
    bool fallback=false;for(auto& item:ed.m_compItems)if(item.label=="value"&&!item.languageRevision)fallback=true;CHECK(fallback);
    publish({{"kind","status"},{"ready",true}});
    ed.SetText(u8"int main(){const char* s=u8\"😀\"; int broken = ;}");ed.RebuildLines();ed.m_cursor=ed.m_text.find("broken")+3;ed.OpenCompletion();
    auto at=ed.m_text.find("broken");auto position=lsp::Position(ed.m_text,at);
    publish({{"kind","diagnostics"},{"file",languageFile},{"revision",languageRevision},{"items",nlohmann::json::array({{{"position",position},{"error",true},{"message","test error"}}})}});
    CHECK(ed.m_diags.size()==1 && ed.m_diags[0].col==int(at+1));
    auto oldRevision=languageRevision;ed.m_text.insert(0," ");publish({{"kind","diagnostics"},{"file",languageFile},{"revision",oldRevision},{"items",nlohmann::json::array()}});CHECK(ed.m_diags.size()==1);
    setup();CHECK(language::Definition(ed));CHECK(languageRequest=="definition");
    publish({{"kind","definition"},{"file",languageFile},{"revision",languageRevision},{"offset",languageOffset},{"target",utils::WToU8(ed.m_path)},{"position",{{"line",0},{"character",7}}}});
    CHECK(g_app.ActiveEditor()==&ed && ed.m_cursor==7);
    language::Reset();CHECK(!language::Ready());
    g_app.editors=std::move(saved);g_app.active=savedIndex;g_app.settings=originalSettings;
}

static void TestCompilerDiscovery() {
    namespace fs=std::filesystem;
    auto root=fs::temp_directory_path()/(L"ccride-compiler-discovery-"+std::to_wstring(GetCurrentProcessId()));
    fs::create_directories(root);
    auto deep=root;for(int i=0;i<14;++i)deep/="nested";
    fs::create_directories(deep);
    fs::create_directories(root/"not-a-compiler/g++.exe");
    auto first=deep/"g++.exe",second=root/"G++.EXE";
    utils::WriteFileU8(first.wstring(),"fixture");utils::WriteFileU8(second.wstring(),"fixture");
    auto results=utils::FindGpps({root.wstring(),root.wstring(),deep.wstring()});
    CHECK(results.size()==2);
    std::atomic<bool> cancelled{true};CHECK(utils::FindGpps({root.wstring()},&cancelled).empty());
    App app;app.settingsPath=(root/"settings.json").wstring();
    app.StartCompilerSearch({root.wstring()});app.compilerScanThread.join();app.PumpCompilerSearch();
    CHECK(app.compilerCandidates.size()==2);CHECK(app.settings.gppPath.empty());CHECK(app.compilerPickerOpen);
    CHECK(app.SelectCompiler(first.wstring()));
    Settings stored;stored.Load(app.settingsPath);CHECK(stored.gppPath==utils::WToU8(first.wstring()));
    app.StartCompilerSearch({root.wstring()});app.compilerScanThread.join();app.PumpCompilerSearch();
    CHECK(app.settings.gppPath==utils::WToU8(first.wstring()));
    CHECK(!app.SelectCompiler((root/"missing.exe").wstring()));
    fs::remove(second);app.settings.gppPath.clear();
    app.StartCompilerSearch({root.wstring()});app.compilerScanThread.join();app.PumpCompilerSearch();
    CHECK(app.settings.gppPath==utils::WToU8(first.wstring()));CHECK(!app.compilerPickerOpen);
    fs::remove_all(root);
}

static void TestOptionalPluginLifecycle() {
    ImGui::CreateContext();
    CodeEditor editor;
    auto complete=[&]() {
        editor.SetText("// optional documentation\nint pluginValue;\nvoid f(){ pluginVal");
        editor.RebuildLines();editor.m_cursor=editor.m_text.size();editor.OpenCompletion();
        for(const auto& item:editor.m_compItems) if(item.label=="pluginValue") return item;
        CHECK(false);return CodeEditor::CompItem{};
    };
    CHECK(!g_app.plugins.HasCompletionProvider());
    auto plain=complete();CHECK(plain.doc.empty() && plain.sources.empty());
    LoadDocumentationTestPlugin();
    auto enhanced=complete();CHECK_EQ(enhanced.doc,"optional documentation");CHECK(!enhanced.sources.empty());
    UnloadDocumentationTestPlugin();
    CHECK(!g_app.plugins.HasCompletionProvider());
    plain=complete();CHECK(plain.doc.empty() && plain.sources.empty());
    ImGui::DestroyContext();
}

static int CheckLocalPluginArchives(const std::filesystem::path& archives,const std::filesystem::path& base=std::filesystem::temp_directory_path()) {
    namespace fs=std::filesystem;
    auto root=base/(L"ccride-local-plugin-check-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
    fs::create_directories(root/"data");g_app.dataDir=(root/"data").wstring();g_app.settingsPath=(root/"data/settings.json").wstring();g_app.pluginDir=(root/"plugins").wstring();
    ImGui::CreateContext();
    auto finish=[&]() {g_app.plugins.UnloadAll();ImGui::DestroyContext();std::error_code e;fs::remove_all(root,e);};
    CHECK(!g_app.plugins.HasCompletionProvider());
    std::string error;
    CHECK(g_app.plugins.InstallLocalArchive((archives/"completion-docs-v1.0.1.zip").wstring(),error));
    CHECK(g_app.plugins.HasCompletionProvider());
    g_app.settings.disabledPlugins.push_back("completion-docs");g_app.plugins.ReloadAll();CHECK(!g_app.plugins.HasCompletionProvider());
    CodeEditor editor;editor.SetText("// plugin comment\nint pluginCount;\nvoid f(){ pluginCou");editor.RebuildLines();editor.m_cursor=editor.m_text.size();editor.OpenCompletion();
    CHECK(!editor.m_compItems.empty());for(auto& item:editor.m_compItems)CHECK(item.doc.empty());
    g_app.settings.disabledPlugins.clear();g_app.plugins.ReloadAll();CHECK(g_app.plugins.HasCompletionProvider());
    auto aiArchive=archives/"ai-assistant-v1.2.1.zip";
    CHECK(g_app.plugins.InstallLocalArchive(aiArchive.wstring(),error));
    CHECK(fs::is_regular_file(root/"plugins/ai-assistant/ai-service.exe"));
    bool aiLoaded=false;for(auto& plugin:g_app.plugins.plugins)if(plugin.id=="ai-assistant" && plugin.loaded)aiLoaded=true;CHECK(aiLoaded);
    CHECK(fs::is_regular_file(root/"data/plugins/ai-assistant/settings.json"));
    CHECK(!g_app.plugins.InstallLocalArchive((archives/"completion-docs-v1.0.1.zip").wstring(),error));
    CHECK(g_app.plugins.Uninstall("completion-docs",error));CHECK(!g_app.plugins.HasCompletionProvider());CHECK(!fs::exists(root/"plugins/completion-docs"));
    CHECK(g_app.plugins.Uninstall("ai-assistant",error));CHECK(g_app.plugins.plugins.empty());
    finish();std::printf("Local plugin packages: %d passed, %d failed\n",g_pass,g_fail);return g_fail?1:0;
}

static int CheckRemotePluginInstall(const std::string& url) {
    namespace fs=std::filesystem;
    auto root=fs::temp_directory_path()/(L"ccride-market-check-"+std::to_wstring(GetCurrentProcessId()));
    fs::create_directories(root);
    g_app.pluginDir=(root/"plugins").wstring();
    ImGui::CreateContext();
    MarketItem item;item.id="remote-docs";item.name="Remote documentation";item.entry="completion-docs.dll";item.apiVersion=2;item.dllUrl=url+"/plugin.dll";std::string digest,error;CHECK(utils::HttpGet(url+"/digest",digest,error));item.sha256=digest;
    g_app.plugins.RequestInstall(item);g_app.plugins.Shutdown();g_app.plugins.PumpNetResults();
    std::string manifest;CHECK(utils::ReadFileU8((root/"plugins/remote-docs/plugin.json").wstring(),manifest));
    auto j=nlohmann::json::parse(manifest);CHECK(j.value("apiVersion",0)==2);
    g_app.plugins.ScanAndLoad();CHECK(g_app.plugins.HasCompletionProvider());
    CHECK(g_app.plugins.plugins.size()==1 && g_app.plugins.plugins[0].loaded);
    g_app.plugins.RequestInstall(item);g_app.plugins.Shutdown();
    CHECK(g_app.plugins.m_msgs.size()==1 && g_app.plugins.m_msgs.front().kind==NetKind::InstallFail);
    g_app.plugins.PumpNetResults();
    for(auto id:{"../escape","CON","bad/name","x:ads"}) {
        item.id=id;g_app.plugins.RequestInstall(item);CHECK(!g_app.plugins.m_netBusy.load());
        CHECK(g_app.plugins.m_msgs.size()==1 && g_app.plugins.m_msgs.front().kind==NetKind::InstallFail);
        g_app.plugins.PumpNetResults();
    }
    item.id="invalid-entry";item.entry="../escape.dll";g_app.plugins.RequestInstall(item);
    CHECK(!fs::exists(root/"plugins/invalid-entry"));CHECK(!g_app.plugins.m_netBusy.load());g_app.plugins.PumpNetResults();
    item.entry="bad.dll";item.apiVersion=99;g_app.plugins.RequestInstall(item);
    CHECK(!g_app.plugins.m_netBusy.load());g_app.plugins.PumpNetResults();
    g_app.plugins.UnloadAll();
    j["apiVersion"]=99;
    utils::WriteFileU8((root/"plugins/remote-docs/plugin.json").wstring(),j.dump());
    g_app.plugins.ScanAndLoad();
    CHECK(g_app.plugins.plugins.size()==1 && !g_app.plugins.plugins[0].loaded && !g_app.plugins.plugins[0].loadError.empty());
    j["apiVersion"]=2;j["entry"]="missing.dll";
    utils::WriteFileU8((root/"plugins/remote-docs/plugin.json").wstring(),j.dump());
    g_app.plugins.ScanAndLoad();
    CHECK(g_app.plugins.plugins.size()==1 && !g_app.plugins.plugins[0].loaded && !g_app.plugins.plugins[0].loadError.empty());
    g_app.plugins.UnloadAll();
    auto oldMarket=g_app.settings.marketUrl;g_app.settings.marketUrl=url+"/catalog.json";
    g_app.plugins.RequestFetchRemote();g_app.plugins.Shutdown();g_app.plugins.PumpNetResults();
    auto found=std::find_if(g_app.plugins.catalog.begin(),g_app.plugins.catalog.end(),[](const MarketItem& x){return x.id=="zip-docs";});
    CHECK(found!=g_app.plugins.catalog.end());
    if(found!=g_app.plugins.catalog.end()){
        MarketItem zip=*found;g_app.plugins.RequestInstall(zip);g_app.plugins.Shutdown();g_app.plugins.PumpNetResults();
        CHECK(fs::is_regular_file(root/"plugins/zip-docs/completion-docs.dll"));
        g_app.plugins.ScanAndLoad();CHECK(g_app.plugins.HasCompletionProvider());g_app.plugins.UnloadAll();
        zip.id="manifest-mismatch";g_app.plugins.RequestInstall(zip);g_app.plugins.Shutdown();CHECK(g_app.plugins.m_msgs.front().kind==NetKind::InstallFail);g_app.plugins.PumpNetResults();CHECK(!fs::exists(root/"plugins/manifest-mismatch"));
        zip.id="bad-checksum";zip.sha256=std::string(64,'0');g_app.plugins.RequestInstall(zip);g_app.plugins.Shutdown();CHECK(g_app.plugins.m_msgs.front().kind==NetKind::InstallFail);g_app.plugins.PumpNetResults();CHECK(!fs::exists(root/"plugins/bad-checksum"));
    }
    std::string body;CHECK(!utils::HttpGet(url+"/plugin.dll",body,error,{},16));CHECK(body.empty());
    CHECK(!utils::HttpGet(url+"/truncated",body,error));CHECK(body.empty());
    g_app.settings.marketUrl=oldMarket;
    CHECK(std::distance(fs::directory_iterator(root/"plugins"),fs::directory_iterator{})==2);
    ImGui::DestroyContext();fs::remove_all(root);
    std::printf("Remote plugin installation: %d passed, %d failed\n",g_pass,g_fail);return g_fail?1:0;
}

static int CheckClangdArchive(const std::filesystem::path& archive,const std::wstring& compiler) {
    namespace fs=std::filesystem;
    auto root=fs::temp_directory_path()/(L"ccride-clangd-package-中文-"+std::to_wstring(GetCurrentProcessId()));
    fs::create_directories(root/"data");g_app.dataDir=(root/"data").wstring();g_app.settingsPath=(root/"data/settings.json").wstring();g_app.pluginDir=(root/"plugins").wstring();
    g_app.settings.gppPath=utils::WToU8(compiler);g_app.settings.codeCompletion=true;g_app.settings.autoCheck=true;
    g_app.editors.clear();g_app.editors.push_back(std::make_unique<CodeEditor>());g_app.active=0;auto& ed=*g_app.editors.front();ed.m_path=(root/L"成员.cpp").wstring();
    ed.SetText("#include <vector>\nusing Nested=std::vector<std::vector<int>>;\nint main(){Nested data; data. }\n");ed.RebuildLines();ed.m_cursor=ed.m_text.find("data.")+5;utils::WriteFileU8(ed.m_path,ed.m_text);
    ImGui::CreateContext();std::string error;CHECK(g_app.plugins.InstallLocalArchive(archive.wstring(),error));
    auto wait=[&](auto predicate,int timeout=15000){auto start=GetTickCount64();while(GetTickCount64()-start<(unsigned)timeout){g_app.plugins.CallFrames();if(predicate())return true;Sleep(15);}return false;};
    CHECK(wait([](){return language::Ready();}));
    ed.OpenCompletion();CHECK(ed.m_languagePending);
    bool hasCompletion=wait([&](){for(auto& item:ed.m_compItems)if(item.insertion=="push_back")return true;return false;});
    CHECK(hasCompletion);
    if(!hasCompletion){
        std::printf("clangd ready=%d items=%zu pending=%d\\n",language::Ready(),ed.m_compItems.size(),ed.m_languagePending);
        for(auto& d:ed.m_diags)std::printf("diagnostic: %s\\n",d.msg.c_str());
        for(auto& line:g_app.console.lines)std::printf("%s",line.text.c_str());
    }
    int idx=0;for(size_t i=0;i<ed.m_compItems.size();++i)if(ed.m_compItems[i].insertion=="push_back")idx=int(i);
    ed.m_compIdx=idx;ed.m_compTouched=true;CHECK(ed.AcceptCompletion());CHECK(ed.m_text.find("data.push_back()")!=std::string::npos);
    ed.SetText("int broken = \"text\";\n");ed.RebuildLines();ed.m_cursor=ed.m_text.size();
    CHECK(wait([&](){return !ed.m_diags.empty();}));
    ed.SetText("struct Box{int value;};\nint main(){Box box; return box.value;}\n");ed.RebuildLines();ed.m_cursor=ed.m_text.rfind("value")+2;
    CHECK(ed.GotoDefinition());CHECK(wait([&](){return ed.m_cursor==ed.m_text.find("value");}));
    bool killed=false;HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);PROCESSENTRY32W process{};process.dwSize=sizeof(process);
    if(snapshot!=INVALID_HANDLE_VALUE){if(Process32FirstW(snapshot,&process))do{if(process.th32ParentProcessID==GetCurrentProcessId()&&_wcsicmp(process.szExeFile,L"clangd.exe")==0){HANDLE child=OpenProcess(PROCESS_TERMINATE,FALSE,process.th32ProcessID);if(child){killed=TerminateProcess(child,42)!=FALSE;CloseHandle(child);}}}while(Process32NextW(snapshot,&process));CloseHandle(snapshot);}
    CHECK(killed);CHECK(wait([](){return !language::Ready();},3000));
    ed.SetText("struct Box{int value;};\nint main(){Box box; box.va}");ed.RebuildLines();ed.m_cursor=ed.m_text.find("box.va")+6;ed.OpenCompletion();
    bool fallback=false;for(auto& item:ed.m_compItems)if(item.label=="value"&&!item.languageRevision)fallback=true;CHECK(fallback);
    g_app.plugins.ReloadAll();CHECK(wait([](){return language::Ready();}));
    CHECK(g_app.plugins.Uninstall("clangd",error));CHECK(!language::Ready());
    ImGui::DestroyContext();g_app.editors.clear();fs::remove_all(root);
    std::printf("Installed clangd integration: %d passed, %d failed\n",g_pass,g_fail);return g_fail?1:0;
}

int main(int argc, char** argv) {
    if(argc==4 && std::string(argv[1])=="--clangd-check") {
        int count=0;auto wide=CommandLineToArgvW(GetCommandLineW(),&count);
        if(!wide || count!=4){if(wide)LocalFree(wide);return 2;}
        std::filesystem::path archive(wide[2]);std::wstring compiler(wide[3]);LocalFree(wide);
        return CheckClangdArchive(archive,compiler);
    }
    if(argc==3 && std::string(argv[1])=="--market-check")return CheckRemotePluginInstall(argv[2]);
    if((argc==3||argc==4) && std::string(argv[1])=="--plugin-check"){
        int count=0;auto wide=CommandLineToArgvW(GetCommandLineW(),&count);
        if(!wide||count!=argc){if(wide)LocalFree(wide);return 2;}
        std::filesystem::path archive(wide[2]);
        auto base=argc==4?std::filesystem::path(wide[3]):std::filesystem::temp_directory_path();
        LocalFree(wide);return CheckLocalPluginArchives(archive,base);
    }
    if (argc == 2 && std::string(argv[1]) == "--package-check") return CheckPortablePackage();
    // 符号索引/文件补全依赖 include 搜索路径: 指向本机编译器 (否则惰性解析会缓存为空)
    if (g_app.settings.gppPath.empty()) {
        const char* cands[] = { "E:\\rred\\mingw64\\bin\\g++.exe",
                                "C:\\mingw64\\bin\\g++.exe",
                                "C:\\msys64\\mingw64\\bin\\g++.exe" };
        for (auto c : cands)
            if (GetFileAttributesA(c) != INVALID_FILE_ATTRIBUTES) {
                g_app.settings.gppPath = c; break;
            }
    }
    std::printf("MiniCPP 回归测试开始...\n");
    TestUtils();
    TestConsoleCap();
    TestLineChanges();
    TestEditorBasics();
    TestEditorUndoRedo();
    TestBookmarkShift();
    TestFindReplace();
    TestTokenizerAndDecl();
    TestNumberToken();
    TestCloseEditorIndex();
    TestCompletionPool();
    TestFileCompletion();
    TestSymbolIndex();
    TestSettingsRoundTrip();
    TestCustomization();
    TestInternationalizationAndCommunity();
    TestIndependentEditorFeatures();
    TestProcStatusThreadSafe();
    TestCommentLinks();
    TestLanguageHost();
    TestMultiCaret();
    TestAllTabRecovery();
    TestPluginBoundaries();
    TestCompilerDiscovery();
    TestOptionalPluginLifecycle();
    TestCompletionDocumentation();
    TestCompletionPopupLayout();
    TestEditorDecorationsAndColors();
    TestNamespaceAndExactCompletion();
    TestQualifiedStreamCompletion();
    TestManagedCompileRun();
    std::printf("通过 %d 项, 失败 %d 项\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
