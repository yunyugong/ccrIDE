#pragma once
#include <imgui.h>

namespace toolbar {
enum class Icon { NewFile, Open, Save, SaveAs, Backup, Undo, Redo, Search, Build, BuildRun, Debug, Run, Stop, Files, Output, AI, Settings };

// Draw at native toolbar size, without relying on symbol fonts or bitmap scaling.
inline void DrawIcon(Icon icon, ImVec2 origin, ImU32 color) {
    auto* d = ImGui::GetWindowDrawList();
    auto p = [&](float x, float y) { return ImVec2(origin.x + x, origin.y + y); };
    auto line = [&](float x, float y, float a, float b) { d->AddLine(p(x,y), p(a,b), color, 1.2f); };
    auto rect = [&](float x, float y, float a, float b) { d->AddRect(p(x,y), p(a,b), color, 1, 0, 1.2f); };
    switch (icon) {
    case Icon::Build: case Icon::BuildRun:
        line(3,14,11,5); line(7,3,13,9); line(6,4,8,2); line(12,10,14,8);
        if(icon==Icon::BuildRun) { line(11,11,15,13); line(15,13,11,15); line(11,15,11,11); } break;
    case Icon::Debug:
        rect(5,4,11,13); line(6,2,10,2); line(2,5,5,7); line(11,7,14,5); line(2,10,5,10); line(11,10,14,10); line(3,15,6,12); line(10,12,13,15); break;
    case Icon::NewFile:
        line(3,1,10,1); line(10,1,14,5); line(14,5,14,15); line(14,15,3,15); line(3,15,3,1);
        line(10,1,10,5); line(10,5,14,5); line(6,10,11,10); line(8.5f,7.5f,8.5f,12.5f); break;
    case Icon::Open:
        line(1,5,1,14); line(1,5,6,5); line(6,5,7,3); line(7,3,13,3); line(13,3,13,7);
        line(1,14,12,14); line(12,14,15,7); line(15,7,4,7); line(4,7,1,14); break;
    case Icon::Save: case Icon::SaveAs:
        rect(2,2,14,14); rect(5,2,11,6); rect(5,10,11,14);
        if (icon == Icon::SaveAs) { line(10,12,15,7); line(9,14,10,12); } break;
    case Icon::Backup:
        rect(2,2,14,14); line(2,6,14,6); line(5,3.5f,11,3.5f); line(6,9,10,9); line(8,8,8,12); break;
    case Icon::Undo: case Icon::Redo: {
        bool reverse = icon == Icon::Redo;
        auto q = [&](float x, float y) { return p(reverse ? 16-x : x,y); };
        d->PathArcTo(p(8,8), 6, reverse ? 0.69f : -2.45f, reverse ? 5.59f : 2.45f, 24); d->PathStroke(color, 0, 1.2f);
        d->AddLine(q(3,4), q(3,8), color, 1.2f); d->AddLine(q(3,8), q(7,8), color, 1.2f);
        d->AddLine(q(3,8), q(9,5), color, 1.2f); break;
    }
    case Icon::Search:
        d->AddCircle(p(6.5f,6.5f), 4.5f, color, 20, 1.2f); line(10,10,15,15); break;
    case Icon::Run:
        d->AddTriangle(p(4,2), p(14,8), p(4,14), color, 1.3f); break;
    case Icon::Stop: rect(3,3,13,13); break;
    case Icon::Files:
        rect(1,2,15,14); line(6,2,6,14); line(9,6,12,6); line(9,9,12,9); break;
    case Icon::Output:
        rect(1,2,15,14); line(4,6,7,8); line(7,8,4,10); line(9,11,12,11); break;
    case Icon::AI:
        rect(3,4,13,13); line(8,1,8,4); d->AddCircleFilled(p(8,1), 1, color);
        d->AddCircleFilled(p(6,8), 1, color); d->AddCircleFilled(p(10,8), 1, color);
        line(6,11,10,11); line(1,7,1,10); line(15,7,15,10); break;
    case Icon::Settings:
        line(2,4,14,4); line(2,8,14,8); line(2,12,14,12);
        rect(5,2,7,6); rect(10,6,12,10); rect(4,10,6,14); break;
    }
}

inline bool Button(const char* id, Icon icon, const char* tip, bool selected = false) {
    ImGui::PushStyleColor(ImGuiCol_Button, selected ? ImGui::GetStyleColorVec4(ImGuiCol_Header) : ImVec4(0,0,0,0));
    bool clicked = ImGui::Button(id, ImVec2(26,26));
    ImGui::PopStyleColor();
    ImVec2 pos = ImGui::GetItemRectMin();
    DrawIcon(icon, ImVec2(pos.x+5, pos.y+5), ImGui::GetColorU32(ImGuiCol_Text));
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tip);
    ImGui::SameLine(0,2);
    return clicked;
}

inline void Separator() {
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddLine(ImVec2(pos.x+4,pos.y+4), ImVec2(pos.x+4,pos.y+22), ImGui::GetColorU32(ImGuiCol_Border));
    ImGui::Dummy(ImVec2(10,26)); ImGui::SameLine(0,2);
}
}
