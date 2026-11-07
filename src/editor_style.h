#pragma once
#include <imgui.h>
#include <array>
#include <cstddef>

enum class EditorColor {
    ActiveBreakpoint, CurrentLine, Breakpoint, WordHighlight, Background, Fold, Gutter, CurrentGutter, Indent, Selection, SelectedText, Text, Caret, LineNumber, Character, Class, Comment, Escape, Float, Function, Global, Identifier, Illegal, Local, Integer, Octal, Hex, Preprocessor, TypeKeyword, Keyword, Whitespace, String, Symbol, Variable, Namespace, Bracket1, Bracket2, Bracket3, Bracket4, Error, Warning, ErrorBg, WarningBg, BracketMatch, Find, AddedLine, OriginalLine, Count
};
struct EditorColorSpec { const char* key; const char* label; const char* group; ImU32 dark, light; };
inline constexpr EditorColorSpec EditorColorSpecs[] = {
    {"ActiveBreakpoint", "调试当前行", "编辑器", IM_COL32(255,190,70,110), IM_COL32(255,190,70,110)},
    {"CurrentLine", "当前行", "编辑器", IM_COL32(37,39,44,255), IM_COL32(240,244,248,255)},
    {"Breakpoint", "断点", "编辑器", IM_COL32(235,80,80,255), IM_COL32(210,45,45,255)},
    {"WordHighlight", "当前高亮单词", "编辑器", IM_COL32(86,156,214,60), IM_COL32(86,156,214,50)},
    {"Background", "编辑器背景", "编辑器", IM_COL32(30,31,35,255), IM_COL32(255,255,255,255)},
    {"Fold", "代码折叠线", "编辑器", IM_COL32(100,120,145,255), IM_COL32(130,140,150,255)},
    {"Gutter", "侧边栏", "编辑器", IM_COL32(30,31,35,255), IM_COL32(242,245,248,255)},
    {"CurrentGutter", "当前行侧边栏", "编辑器", IM_COL32(44,48,57,255), IM_COL32(221,233,246,255)},
    {"Indent", "缩进提示线", "编辑器", IM_COL32(80,97,119,120), IM_COL32(135,150,167,130)},
    {"Selection", "选中背景", "编辑器", IM_COL32(48,83,120,255), IM_COL32(173,214,255,255)},
    {"SelectedText", "选中文字", "编辑器", IM_COL32(255,255,255,255), IM_COL32(18,31,48,255)},
    {"Text", "编辑器默认", "编辑器", IM_COL32(212,220,230,255), IM_COL32(22,30,40,255)},
    {"Caret", "光标", "编辑器", IM_COL32(245,248,255,255), IM_COL32(20,30,40,255)},
    {"LineNumber", "行号", "编辑器", IM_COL32(95,111,130,255), IM_COL32(135,143,153,255)},
    {"Character", "字符", "语法", IM_COL32(216,166,126,255), IM_COL32(163,70,35,255)},
    {"Class", "类", "语法", IM_COL32(78,201,176,255), IM_COL32(0,128,128,255)},
    {"Comment", "注释", "语法", IM_COL32(112,157,96,255), IM_COL32(50,128,45,255)},
    {"Escape", "转义序列", "语法", IM_COL32(231,189,118,255), IM_COL32(174,90,15,255)},
    {"Float", "浮点数", "语法", IM_COL32(181,206,168,255), IM_COL32(30,132,95,255)},
    {"Function", "函数", "语法", IM_COL32(220,210,160,255), IM_COL32(120,82,32,255)},
    {"Global", "全局变量", "语法", IM_COL32(158,198,235,255), IM_COL32(20,85,150,255)},
    {"Identifier", "标识符", "语法", IM_COL32(212,220,230,255), IM_COL32(22,30,40,255)},
    {"Illegal", "非法字符", "语法", IM_COL32(255,100,115,255), IM_COL32(210,30,45,255)},
    {"Local", "局部变量", "语法", IM_COL32(156,220,254,255), IM_COL32(0,100,155,255)},
    {"Integer", "整数", "语法", IM_COL32(181,206,168,255), IM_COL32(30,132,95,255)},
    {"Octal", "八进制整数", "语法", IM_COL32(163,196,154,255), IM_COL32(70,130,70,255)},
    {"Hex", "十六进制整数", "语法", IM_COL32(185,202,140,255), IM_COL32(90,120,45,255)},
    {"Preprocessor", "预处理指令", "语法", IM_COL32(197,134,192,255), IM_COL32(135,50,150,255)},
    {"TypeKeyword", "类型关键字", "语法", IM_COL32(86,156,214,255), IM_COL32(0,85,190,255)},
    {"Keyword", "关键字", "语法", IM_COL32(197,134,192,255), IM_COL32(110,40,165,255)},
    {"Whitespace", "空格字符", "语法", IM_COL32(92,107,125,255), IM_COL32(155,165,175,255)},
    {"String", "字符串", "语法", IM_COL32(206,145,120,255), IM_COL32(163,45,35,255)},
    {"Symbol", "符号", "语法", IM_COL32(184,196,210,255), IM_COL32(70,80,95,255)},
    {"Variable", "变量／成员", "语法", IM_COL32(156,200,230,255), IM_COL32(40,95,140,255)},
    {"Namespace", "命名空间", "语法", IM_COL32(140,180,230,255), IM_COL32(60,95,160,255)},
    {"Bracket1", "1 级括号", "语法", IM_COL32(230,190,100,255), IM_COL32(155,105,10,255)},
    {"Bracket2", "2 级括号", "语法", IM_COL32(190,135,230,255), IM_COL32(130,55,170,255)},
    {"Bracket3", "3 级括号", "语法", IM_COL32(100,190,220,255), IM_COL32(15,125,165,255)},
    {"Bracket4", "4 级括号", "语法", IM_COL32(125,205,155,255), IM_COL32(30,140,75,255)},
    {"Error", "错误标记", "语法检查", IM_COL32(235,80,80,255), IM_COL32(210,45,45,255)},
    {"Warning", "警告标记", "语法检查", IM_COL32(230,190,70,255), IM_COL32(175,130,25,255)},
    {"ErrorBg", "错误行背景", "语法检查", IM_COL32(140,40,40,56), IM_COL32(230,40,40,35)},
    {"WarningBg", "警告行背景", "语法检查", IM_COL32(130,100,25,48), IM_COL32(230,170,20,35)},
    {"BracketMatch", "匹配括号背景", "编辑器", IM_COL32(90,150,230,60), IM_COL32(60,120,210,50)},
    {"Find", "查找匹配", "编辑器", IM_COL32(255,190,60,90), IM_COL32(255,220,0,90)},
    {"AddedLine", "新增或修改行（红杠）", "编辑器", IM_COL32(240,80,85,255), IM_COL32(210,45,50,255)},
    {"OriginalLine", "原有或已保存行（绿杠）", "编辑器", IM_COL32(60,190,120,255), IM_COL32(30,150,80,255)},
};
static_assert(sizeof(EditorColorSpecs) / sizeof(EditorColorSpec) == (size_t)EditorColor::Count);
inline auto DefaultEditorPalette(bool dark) {
    std::array<ImU32, (size_t)EditorColor::Count> colors{};
    for (size_t i=0; i<colors.size(); ++i) colors[i]=dark ? EditorColorSpecs[i].dark : EditorColorSpecs[i].light;
    return colors;
}
