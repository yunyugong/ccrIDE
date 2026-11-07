#pragma once
// MiniCPP - ImGui 与 std::string 的输入辅助 (imgui 未内置该重载)
#include <string>
#include <cstring>
#include <imgui.h>

namespace ui {

static int StdStringResizeCallback(ImGuiInputTextCallbackData* data) {
    std::string* str = (std::string*)data->UserData;
    if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        std::string ns(data->Buf, data->BufTextLen);
        *str = std::move(ns);
        data->Buf = (char*)str->data();
        data->BufSize = (int)str->size() + 1;
    }
    return 0;
}

static inline bool InputTextStd(const char* label, std::string& str,
                                ImGuiInputTextFlags flags = 0) {
    flags |= ImGuiInputTextFlags_CallbackResize;
    bool r = ImGui::InputText(label, (char*)str.data(), str.capacity() + 1, flags,
                              StdStringResizeCallback, &str);
    str.resize(strlen(str.data()));
    return r;
}

static inline bool InputTextMultilineStd(const char* label, std::string& str,
                                         const ImVec2& size = ImVec2(0, 0),
                                         ImGuiInputTextFlags flags = 0) {
    flags |= ImGuiInputTextFlags_CallbackResize;
    bool r = ImGui::InputTextMultiline(label, (char*)str.data(), str.capacity() + 1, size,
                                       flags, StdStringResizeCallback, &str);
    str.resize(strlen(str.data()));
    return r;
}

} // namespace ui
