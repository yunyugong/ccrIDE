#pragma once
// MiniCPP - 轻量 C++ 符号索引器 (类似小熊猫C++ 的做法: 解析头文件源码提取符号,
// 而不是手写成员表)。解析当前打开的文档 + g++ include 路径下的标准头文件,
// 从 class/struct 定义体中提取方法与字段, 供补全使用。
#include <string>
#include <vector>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>

struct SymRef {
    std::string name;   // 成员名
    bool isFunc;        // true=方法(后跟 "(" ), false=字段
    std::string sig;    // 函数签名 (仅方法): "name(参数列表)"
};

// 文档引用: uid 唯一标识一个打开的编辑器 (关闭标签后残留 key 不再泄漏/误配)
struct DocRef {
    uint64_t           uid  = 0;
    const std::string* text = nullptr;
};

class SymbolIndex {
public:
    static SymbolIndex& I();

    // 取类型成员: 1) 打开文档中定义的类  2) 对应系统头文件  3) (空则由调用方回退内置表)
    // normType 已归一化 (去 std:: / 模板参数 / const / &*)
    std::vector<SymRef> MembersOfType(const std::string& normType,
                                      const std::vector<DocRef>& docs);

    // g++ 的 include 搜索路径 (首次调用时解析并缓存), 供 #include 文件补全使用
    const std::vector<std::wstring>& IncludeDirs();

    // 从源码文本提取 #include 头文件名 (按出现顺序)
    static std::vector<std::string> ExtractIncludeNames(const std::string& text);

    // 一行在"翻译单元"文本中的来源 (用于把符号位置映射回 真实文件+行号)
    struct TuSegment {
        size_t outStart = 0;   // 该行在 TU 文本中的起始偏移
        size_t outLen = 0;     // 该行长度
        int    fileId = -1;    // files[fileId]
        int    line = 1;       // 源文件中的行号 (1-based)
    };
    // 递归内联生成的头文件"翻译单元" (仿 CppPreprocessor)
    struct HeaderUnit {
        std::string text;                     // 内联后的文本 (供解析)
        std::vector<std::wstring> files;      // fileId -> 路径
        std::vector<TuSegment> segments;      // 每行一段 (outStart 递增)
        bool Locate(size_t off, std::wstring& file, int& line) const;
    };

    // 递归内联 docText 中所有可解析的 #include 头文件内容 (docPath 为文档路径,
    // 用于 "..." 相对包含与来源映射), 生成 HeaderUnit。maxBytes 限制总量。
    HeaderUnit BuildHeaderUnit(const std::string& docText, const std::wstring& docPath,
                               size_t maxBytes = 12u * 1024 * 1024);

    // 是否已配置编译器 include 路径 (无则无法解析头文件, 补全回退内置表)
    bool HasIncludeDirs() { return !IncludeDirs().empty(); }

private:
    SymbolIndex() = default;

    struct ClassInfo {
        std::string name;                 // 短名 (vector)
        std::vector<std::string> bases;   // 基类短名
        std::vector<SymRef> members;
    };

    std::unordered_map<std::string, ClassInfo> m_classes;      // 短名 -> 类
    std::unordered_map<std::string, std::string> m_alias;      // using string = basic_string
    std::unordered_set<std::string> m_parsedHeaders;           // 已解析的系统头
    std::unordered_set<std::string> m_noHeader;                // 找不到对应头的类型 (负缓存)
    std::unordered_map<uint64_t, size_t> m_docHash;            // 编辑器 uid -> 内容hash (脏检查)

    bool m_dirsDone = false;
    std::vector<std::wstring> m_dirs;                          // include 搜索路径

    void EnsureIncludeDirs();
    bool ParseSystemHeader(const std::string& headerName, int depth = 0);
    void ParseText(const std::string& src);
    void ScanDoc(uint64_t uid, const std::string* doc);
    const ClassInfo* FindClass(const std::string& shortName, int depth = 0);
    void CollectChain(const std::string& shortName, std::vector<SymRef>& out,
                      std::unordered_set<std::string>& seen, int depth);
};
