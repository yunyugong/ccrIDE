#pragma once
// MiniCPP - 文档级 C++ 语义模型 (轻量解析, 面向代码补全)
// 思路对齐小熊猫C++: 补全基于"代码本身"建立的符号表, 而非仅靠头文件/词法。
// 提供:
//   1) 作用域感知的符号补全 (只提示光标处可见的局部变量/参数/函数/类型)
//   2) 更准的对象类型推断 (a. / a-> / 链式 a.b / auto / 函数参数 / this->)
//   3) 用户自定义类型 (class/struct/enum/typedef/using) 的成员补全 (含基类链)
//   4) 用户函数/成员函数的签名提示
// 不追求完整语法分析, 只保证对常见代码有足够好的提取效果。
#include <string>
#include <vector>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>

// 符号类别 (供补全列表着色/图标)
enum class CSymKind : uint8_t {
    Variable, Parameter, Function, Type, Enum, Enumerator, Macro, Namespace, Member
};

struct CSym {
    std::string name;
    std::string ns;        // 所在命名空间 (如 "std" / "foo::bar"), 全局为空
    std::string type;      // 变量/参数/成员: 声明类型(已归一化); 函数: 返回类型
    std::string sig;       // 函数: "name(args)"; 其他为空
    CSymKind    kind = CSymKind::Variable;
    size_t      declPos = 0;
    size_t      scopeStart = 0, scopeEnd = 0;  // 可见范围 (局部符号)
    bool        isLocal = false;
    bool        defined = false;  // 类型: true=有定义体 (class X {...}); false=仅前置声明 (class X;)
    std::string initExpr;  // auto 变量的初始化表达式 (惰性推断用)
};

struct CMember {
    std::string name;
    std::string type;      // 方法为返回类型, 字段为声明类型
    std::string sig;       // 方法签名 "name(args)"
    CSymKind    kind = CSymKind::Member;
    int         access = 0;   // 0 public, 1 protected, 2 private (简化)
    size_t      declPos = 0;  // 声明处 byte offset (跳转定义用)
};

struct CTypeInfo {
    std::string name;       // 短名
    bool        ok = false;
    bool        isEnum = false;
    bool        isStruct = false;
    std::vector<CMember> members;
    std::vector<std::string> bases;   // 基类短名 (已归一化)
};

class CodeModel {
public:
    void Ensure(const std::string& text);   // 内容变化时才重建
    void Rebuild(const std::string& text);

    // 解析系统头文件内容时置 true: 过滤 libstdc++ 内部名 (_M_/__xxx), 合并重复类定义
    void SetSystemSource(bool on) { m_system = on; }
    bool IsSystemSource() const { return m_system; }

    // 光标处可见的符号 (全局 + 作用域内局部 + 所在类成员), 供词补全
    void VisibleSymbols(size_t cursorPos, std::vector<CSym>& out) const;
    // Apply this document's namespace/using context to declarations from another tab.
    void FilterVisibleSymbols(size_t cursorPos, const std::vector<CSym>& candidates,
                              std::vector<CSym>& out) const;

    // name 在 cursorPos 处的类型 (归一化). 找不到返回 ""
    std::string ResolveVarType(const std::string& name, size_t cursorPos) const;

    // 解析访问表达式 (如 "a.b->c" / "this->x" / "Foo::bar"), 返回结果的归一化类型
    std::string EvalExprType(const std::string& expr, size_t cursorPos) const;

    // 用户类型 (含别名解析). 未定义返回 nullptr
    const CTypeInfo* FindType(const std::string& typeName) const;
    // 用户类型成员 (含基类链, 去重). 返回是否找到该类型
    bool CollectMembers(const std::string& typeName, std::vector<CMember>& out) const;

    // 用户函数签名查找 (objType 非空 => 查该类型的成员函数)
    bool FindFunctionSig(const std::string& name, const std::string& objType,
                         std::string& sig, std::string& retType) const;

    // 跳转定义: 求 name 在 cursorPos 处的声明偏移 (作用域感知). 找到返回 true
    bool FindDeclPos(const std::string& name, size_t cursorPos, size_t& outPos) const;
    // 跳转定义(成员访问 obj.member): 求 typeName 类型的成员 member 的声明偏移
    bool FindMemberDeclPos(const std::string& typeName, const std::string& member, size_t& outPos) const;

    // 光标处是否位于某个类的方法体内 -> 返回类名 (this 类型), 否则空
    std::string ThisTypeAt(size_t cursorPos) const { return ThisTypeAtImpl(cursorPos); }

    static std::string NormType(std::string t);

    const std::vector<CSym>& Globals() const { return m_globals; }
    const std::vector<CSym>& Locals() const { return m_locals; }
    // 全部符号 (全局函数/变量/枚举值 + 类型名/别名), 供命名空间/词补全使用
    std::vector<CSym> AllSymbols() const;
    // 指定命名空间下的符号 (includeSub=true 时含子命名空间: std::__cxx11 也算 std)
    void NamespaceSymbols(const std::string& ns, bool includeSub, std::vector<CSym>& out) const;
    void DeclarationSites(const CSym& symbol, std::vector<CSym>& out) const;
    bool Empty() const { return m_toks.empty(); }

private:
    struct Tok {
        enum K : uint8_t { Ident, Number, Punct, Str, Char } k = Punct;
        std::string s;
        size_t pos = 0, end = 0;
        int line = 0;
    };
    struct ClassScope { size_t start, end; std::string cls; };
    struct FuncScope  { size_t start, end; std::string thisType; };
    struct NamespaceScope { size_t start, end; std::string name; };
    struct UsingImport {
        std::string target, owner;
        size_t declPos, start, end;
        bool namespc, local, absolute;
    };

    std::string m_text;
    std::vector<Tok> m_toks;
    size_t m_hash = 0;
    bool   m_valid = false;

    std::vector<CSym> m_globals;
    std::vector<CSym> m_declarationSites;
    std::vector<CSym> m_locals;
    std::unordered_map<std::string, CTypeInfo> m_types;     // 短名 -> 类型
    std::unordered_map<std::string, std::string> m_aliases; // 别名 -> 目标短名
    std::unordered_map<std::string, size_t> m_aliasPos;     // 别名 -> 声明偏移 (跳转用)
    std::unordered_set<std::string> m_knownTypes;           // 内置 + 已定义类型 + 模板参数

    std::vector<ClassScope> m_classScopes;
    std::vector<FuncScope>  m_funcScopes;
    std::vector<NamespaceScope> m_namespaceScopes;
    std::vector<UsingImport> m_usingImports;
    std::unordered_set<std::string> m_inlineNamespaces;

    std::unordered_set<std::string> m_namespaces; // Fully qualified namespace names, separate from types.

    bool m_system = false;    // 系统头来源 (过滤内部名/合并重复类)
    mutable int m_queryDepth = 0;

    void Tokenize(const std::string& text);
    std::string ThisTypeAtImpl(size_t cursorPos) const;
};
