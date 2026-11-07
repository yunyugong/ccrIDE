#include <windows.h>
#include <shellapi.h>
#include <imgui.h>
#include <imgui_internal.h>
#include "../../src/plugin_api.h"
#include "completion_docs.h"
#include "comment_links.h"
#include <memory>
#include <unordered_map>
static PluginApi* api=nullptr;
struct CachedDoc { std::string text; std::unique_ptr<CompletionDocIndex> index; };
static std::unordered_map<const char*,CachedDoc> cache;
static std::wstring ToWide(const std::string& value) {
    int n=MultiByteToWideChar(CP_UTF8,0,value.data(),(int)value.size(),nullptr,0);
    std::wstring out(n,0); MultiByteToWideChar(CP_UTF8,0,value.data(),(int)value.size(),out.data(),n); return out;
}
// Draw wrapped documentation with clickable HTTP(S) links, including wrapped URL fragments.
static void DrawCompletionDoc(const std::string& text) {
    const auto links = FindCommentLinks(text);
    if (links.empty()) { ImGui::TextUnformatted(text.c_str()); return; }
    ImFont* font = ImGui::GetFont();
    const float size = ImGui::GetFontSize();
    const float width = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    const char* begin = text.data();
    const char* end = begin + text.size();
    const char* line = begin;
    ImGui::PushTextWrapPos(-1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
    while (line < end) {
        const char* stop = font->CalcWordWrapPosition(size, line, end, width);
        ImVec2 origin = ImGui::GetCursorScreenPos();
        if (stop == line) ImGui::NewLine(); // explicit empty line
        else ImGui::TextUnformatted(line, stop);
        ImVec2 next = ImGui::GetCursorScreenPos();
        for (size_t k = 0; k < links.size(); ++k) {
            const auto& link = links[k];
            const char* a = std::max(line, begin + link.begin);
            const char* b = std::min(stop, begin + link.end);
            if (a >= b) continue;
            float x = font->CalcTextSizeA(size, FLT_MAX, -1, line, a).x;
            float w = font->CalcTextSizeA(size, FLT_MAX, -1, a, b).x;
            ImVec2 pos(origin.x + x, origin.y);
            ImGui::SetCursorScreenPos(pos);
            ImGui::PushID((int)k);
            ImGui::PushID((int)(a - begin));
            bool clicked = ImGui::InvisibleButton("##comment_link", ImVec2(std::max(1.0f, w), size));
            bool hovered = ImGui::IsItemHovered();
            ImGui::PopID(); ImGui::PopID();
            ImU32 color = ImGui::GetColorU32(ImGuiCol_TextLink);
            auto* draw = ImGui::GetWindowDrawList();
            draw->AddText(font, size, pos, color, a, b);
            draw->AddLine(ImVec2(pos.x, pos.y + size), ImVec2(pos.x + w, pos.y + size), color);
            if (hovered) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                ImGui::SetTooltip("%s", link.url.c_str());
            }
            if (clicked)
                ShellExecuteW(nullptr, L"open", ToWide(link.url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        ImGui::SetCursorScreenPos(next);
        if (stop == end) break;
        // Match ImGui wrapping: discard blanks at a soft wrap, preserve explicit newlines.
        if (*stop == '\n') line = stop + 1;
        else {
            line = stop;
            while (line < end && (*line == ' ' || *line == '\t')) ++line;
            if (line < end && *line == '\n') ++line;
        }
    }
    ImGui::PopStyleVar();
    ImGui::PopTextWrapPos();
}


static void Begin() { cache.clear(); }
static void Describe(const char* text,size_t size,size_t offset,PluginTextSink sink,void* user) {
    auto& entry=cache[text];
    if(!entry.index) { entry.text.assign(text,size); entry.index=std::make_unique<CompletionDocIndex>(entry.text); }
    auto doc=CompletionDocBefore(entry.text,offset,entry.index.get()); sink(user,doc.data(),doc.size());
}
static void Draw(const PluginCompletionView* view) {
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(*view->hint ? view->hint : view->label);
    if(view->sourceCount) {
        int& chosen=*view->selectedSource; chosen=std::clamp(chosen,0,(int)view->sourceCount-1);
        auto label=[](const PluginCompletionSource& source) {
            std::string file=source.file; auto sep=file.find_last_of("/\\");
            if(sep!=std::string::npos) file.erase(0,sep+1);
            return (file.empty() ? std::string("当前文档") : file)+":"+std::to_string(source.line);
        };
        ImGui::TextDisabled("声明来源 · %zu 处",view->sourceCount); ImGui::SetNextItemWidth(-FLT_MIN);
        auto* window=ImGui::GetCurrentWindow();
        if(ImGui::BeginCombo("##completion_source",label(view->sources[chosen]).c_str())) {
            for(int i=0;i<(int)view->sourceCount;++i) {
                ImGui::PushID(i);
                if(ImGui::Selectable(label(view->sources[i]).c_str(),i==chosen)) { chosen=i; ImGui::SetScrollY(window,0); }
                if(ImGui::IsItemHovered()) ImGui::SetTooltip("%s",view->sources[i].file);
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("%s",view->sources[chosen].file);
        if(*view->sources[chosen].documentation) { ImGui::Spacing(); DrawCompletionDoc(view->sources[chosen].documentation); }
        else ImGui::TextDisabled("此处没有声明注释，可切换其他来源查看。");
    }
    ImGui::PopTextWrapPos();
}
extern "C" __declspec(dllexport) void Plugin_OnLoadV2(void* context,PluginApi* host) {
    api=host; ImGui::SetCurrentContext((ImGuiContext*)context);
    ImGui::SetAllocatorFunctions(api->Allocate,api->Deallocate,api->allocatorUser);
    static const PluginCompletionHooks hooks{Begin,Describe,Draw}; api->RegisterCompletion(&hooks);
}
extern "C" __declspec(dllexport) void Plugin_OnFrame() {}
extern "C" __declspec(dllexport) void Plugin_OnUnload() { cache.clear(); api=nullptr; }
