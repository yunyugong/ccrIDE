#pragma once
#include <filesystem>
#include <string>

namespace portable {
inline std::wstring Resolve(const std::wstring& root, const std::wstring& path) {
    if (path.empty()) return {};
    std::filesystem::path value(path);
    return (value.is_absolute() ? value : std::filesystem::path(root) / value).lexically_normal().wstring();
}
inline std::wstring Store(const std::wstring& root, const std::wstring& path) {
    if (root.empty() || path.empty()) return path;
    auto relative = std::filesystem::path(path).lexically_relative(std::filesystem::path(root));
    if (!relative.empty() && *relative.begin() != L"..") return relative.generic_wstring();
    return path;
}
}
