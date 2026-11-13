// Render the real workspace into a hidden GLFW framebuffer for visual/layout QA.
#define main MiniCppApplicationMain
#define WinMain MiniCppWindowsMain
#include "../src/main.cpp"
#undef WinMain
#undef main
#include <fstream>
#include "plugin_test_support.h"
#include <cmath>
#include "language_host.h"
#include <nlohmann/json.hpp>

static void SaveFrame(const std::filesystem::path& path, int width, int height) {
    int stride = (width * 3 + 3) & ~3;
    std::vector<unsigned char> rgb(width * height * 3), bgr(stride * height);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, rgb.data());
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            for (int c = 0; c < 3; ++c) bgr[y * stride + x * 3 + c] = rgb[(y * width + x) * 3 + 2 - c];
    BITMAPFILEHEADER file{};
    BITMAPINFOHEADER info{};
    file.bfType = 0x4d42; file.bfOffBits = sizeof(file) + sizeof(info); file.bfSize = file.bfOffBits + bgr.size();
    info.biSize = sizeof(info); info.biWidth = width; info.biHeight = height;
    info.biPlanes = 1; info.biBitCount = 24; info.biSizeImage = bgr.size();
    std::ofstream out(path, std::ios::binary);
    out.write((char*)&file, sizeof(file)); out.write((char*)&info, sizeof(info));
    out.write((char*)bgr.data(), bgr.size());
}

int main(int argc, char** argv) {
    CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    if ((argc != 2 && argc != 3) || !glfwInit()) return 2;
    std::filesystem::path output(argv[1]);
    std::filesystem::create_directories(output);
    char previewLanguage[32]{};GetEnvironmentVariableA("CCRIDE_PREVIEW_LANGUAGE",previewLanguage,sizeof(previewLanguage));
    const bool englishPreview=std::string(previewLanguage)=="en";
    g_app.settings.language=englishPreview?"en":"zh-CN";i18n::SetLanguage(g_app.settings.language);
    int failures = 0;
    for (int scenario = 0; scenario < 3; ++scenario) {
        int width = scenario == 2 ? 640 : 1280, height = scenario == 2 ? 480 : 840;
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3); glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        GLFWwindow* window = glfwCreateWindow(width, height, "MiniCPP preview", nullptr, nullptr);
        if (!window) return 3;
        glfwMakeContextCurrent(window);
        ImGui::CreateContext();
        auto& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable; io.IniFilename = nullptr;
        ImGui_ImplGlfw_InitForOpenGL(window, true); ImGui_ImplOpenGL3_Init("#version 130");
        g_app.settings.gppPath = "E:\\rred\\mingw64\\bin\\g++.exe";
        if(HotkeyText("compile")!="F10"||HotkeyText("run")!="F11"||HotkeyText("runOnly")!="F12") { ++failures; printf("FAIL default build shortcuts\n"); }
        g_app.settings.theme = scenario == 1 ? "light" : "dark";
        g_app.LoadFonts(); g_app.ApplyTheme();
        g_app.showFileTree = true; g_app.showConsole = true;
        g_app.editors.clear(); g_app.active = -1;
        g_app.NewFile(); g_app.ActiveEditor()->SetPath(L"E:\\cppIDE\\src\\main.cpp");
        g_app.ActiveEditor()->SetText("#include <iostream>\n#include <vector>\n\n// A small C++ program\nint main() {\n    std::vector<int> numbers = {1, 2, 3, 4, 5};\n\n    for (int value : numbers) {\n        std::cout << value << '\\n';\n    }\n\n    return 0;\n}\n");
        g_app.NewFile(); g_app.ActiveEditor()->SetPath(L"E:\\cppIDE\\src\\utils.h");
        g_app.ActiveEditor()->SetText("#pragma once\n");
        g_app.active = 0;
        g_app.console.Clear();
        g_resetWorkspace = true;
        const uint64_t originalMainUid=g_app.editors[0]->m_uid;
        ImVec2 testPointer(-100, -100);
        for (int frame = 0; frame < 17; ++frame) {
            glfwPollEvents();
            ImGui_ImplOpenGL3_NewFrame(); ImGui_ImplGlfw_NewFrame();
            io.AddFocusEvent(true);
            if (frame == 6) g_app.active = 1; // external navigation/shortcut
            if (frame == 8 || frame == 13) {
                auto* editor = ImGui::FindWindowByName("编辑器");
                auto* bar = ImGui::GetCurrentContext()->TabBars.GetByKey(ImHashStr("##files", 0, editor->ID));
                bool found = false;
                if (bar) for (auto& tab : bar->Tabs) {
                    const char* name = ImGui::TabBarGetTabName(bar, &tab);
                    if ((frame == 8 && strcmp(name, "+") == 0) ||
                        (frame == 13 && strstr(name, i18n::Tr("未命名.cpp")))) {
                        testPointer = ImVec2(bar->BarRect.Min.x + tab.Offset + tab.Width * 0.5f,
                                             bar->BarRect.Min.y + bar->BarRect.GetHeight() * 0.5f);
                        io.AddMousePosEvent(testPointer.x, testPointer.y);
                        found = true; break;
                    }
                }
                if (!found) { ++failures; printf("FAIL scenario %d: tab target unavailable\n", scenario); }
            }
            if (frame >= 8) io.AddMousePosEvent(testPointer.x, testPointer.y);
            if (frame == 9) io.AddMouseButtonEvent(0, true);
            if (frame == 10) io.AddMouseButtonEvent(0, false);
            if (frame == 14) io.AddMouseButtonEvent(2, true);
            if (frame == 15) io.AddMouseButtonEvent(2, false);
            ImGui::NewFrame();
            DrawMainMenuBar(); DrawWorkspaceToolbar(); DrawStatusBar(); DrawActivityBar(); DrawWorkspaceDock();
            DrawFileTreeWindow(); g_app.console.DrawWindow(&g_app.showConsole);
            DefaultBottomDock(); DrawProblemsWindow();
            DefaultBottomDock(); g_app.terminal.DrawWindow(&g_app.showTerminal,true);
            DrawEditorWindow();
            ImGui::Render(); glViewport(0, 0, width, height);
            glClearColor(0.08f,0.1f,0.13f,1); glClear(GL_COLOR_BUFFER_BIT);
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
            if (frame == 5) {
                auto* editor = ImGui::FindWindowByName("编辑器");
                auto* toolbar = ImGui::FindWindowByName("##workspace_toolbar");
                auto* status = ImGui::FindWindowByName("##statusbar");
                auto* console = ImGui::FindWindowByName("输出");
                auto check = [&](bool condition, const char* name) {
                    if (!condition) { ++failures; printf("FAIL scenario %d: %s\n", scenario, name); }
                };
                check(editor && toolbar && status && console, "workspace windows exist");
                if (editor && toolbar && status && console) {
                    check(editor->Pos.y >= toolbar->Pos.y + toolbar->Size.y - 1, "toolbar does not cover editor");
                    check(console->Pos.y + console->Size.y <= status->Pos.y + 1, "status does not cover output");
                    check(editor->Size.x >= width * 0.55f, "editor has most horizontal space");
                    check(toolbar->ScrollMax.x <= 1, "toolbar controls fit");
                }
                auto* problems=ImGui::FindWindowByName("问题");
                auto* terminal=ImGui::FindWindowByName("终端");
                if(!problems || !terminal || problems->DockId!=console->DockId || terminal->DockId!=console->DockId) { ++failures; printf("FAIL default bottom tabs\n"); }
                if(!g_shellIcons.Get(L"E:\\cppIDE\\src\\main.cpp") || !g_shellIcons.TextureCount()) { ++failures; printf("FAIL shell file association icon\n"); }
                SaveFrame(output / (scenario == 0 ? "ide-dark.bmp" : scenario == 1 ? "ide-light.bmp" : "ide-compact.bmp"), width, height);
            }
            if (frame == 7 && g_app.active != 1) { ++failures; printf("FAIL external tab selection\n"); }
            if (frame == 11 && (g_app.editors.size() != 3 || g_app.active != 2)) { ++failures; printf("FAIL new tab selection: count=%d active=%d\n", (int)g_app.editors.size(), g_app.active); }
            if (frame == 16 && (g_app.editors.size() != 2 || g_app.editors[0]->m_uid != originalMainUid)) { ++failures; printf("FAIL middle-click close\n"); }
            glfwSwapBuffers(window);
        }
        // A live language change must reuse existing docked windows and IDs.
        auto* languageEditor=ImGui::FindWindowByName("编辑器");
        const auto oldDock=languageEditor->DockId;const auto oldId=languageEditor->ID;
        for(const char* language:{"en","zh-CN",englishPreview?"en":"zh-CN"}){
            i18n::SetLanguage(language);g_app.settings.language=language;
            ImGui_ImplOpenGL3_NewFrame();ImGui_ImplGlfw_NewFrame();ImGui::NewFrame();
            DrawMainMenuBar();DrawWorkspaceToolbar();DrawStatusBar();DrawActivityBar();DrawWorkspaceDock();
            DrawFileTreeWindow();g_app.console.DrawWindow(&g_app.showConsole);DefaultBottomDock();DrawProblemsWindow();DefaultBottomDock();g_app.terminal.DrawWindow(&g_app.showTerminal,true);DrawEditorWindow();ImGui::Render();
            auto* current=ImGui::FindWindowByName("编辑器");
            if(current!=languageEditor||current->ID!=oldId||current->DockId!=oldDock){++failures;printf("FAIL language switch changed docking\n");}
        }
        if(scenario==0) {
            LoadDocumentationTestPlugin();
            g_app.active=0;
            auto* completion=g_app.ActiveEditor();
            ImVec2 pointer(-100,-100);
            for(int variant=0;variant<2;++variant) {
                completion->SetText(variant ? "#include <iostream>\n\nint main() {\n    std" : "#include <iostream>\n\nint main() {\n    std::ci");
                completion->RebuildLines(); completion->m_cursor=completion->m_text.size(); completion->OpenCompletion();
                const char* name=variant ? "std" : "cin";
                for(size_t i=0;i<completion->m_compItems.size();++i) if(completion->m_compItems[i].label==name) completion->m_compIdx=(int)i;
                if(completion->m_compItems.empty()) { ++failures; printf("FAIL real header completion preview\n"); continue; }
                if(variant && completion->m_compItems[completion->m_compIdx].sources.size()<2) { ++failures; printf("FAIL namespace header choices\n"); }
                for(int frame=0;frame<(variant ? 12 : 4);++frame) {
                    ImGui_ImplOpenGL3_NewFrame(); ImGui_ImplGlfw_NewFrame(); io.AddFocusEvent(true);
                    io.AddMousePosEvent(pointer.x,pointer.y);
                    if(variant && (frame==3 || frame==7)) io.AddMouseButtonEvent(0,true);
                    if(variant && (frame==4 || frame==8)) io.AddMouseButtonEvent(0,false);
                    ImGui::NewFrame(); DrawMainMenuBar(); DrawWorkspaceToolbar(); DrawStatusBar(); DrawActivityBar(); DrawWorkspaceDock();
                    DrawFileTreeWindow(); g_app.console.DrawWindow(&g_app.showConsole);
                    DefaultBottomDock(); DrawProblemsWindow();
                    DefaultBottomDock(); g_app.terminal.DrawWindow(&g_app.showTerminal,true); DrawEditorWindow();
                    if(variant && frame==2) {
                        auto* popup=ImGui::FindWindowByName("##completion");
                        for(auto* child:ImGui::GetCurrentContext()->Windows) if(child->ParentWindow==popup) {
                            float font=g_app.editorFont->LegacySize;
                            pointer=ImVec2(child->DC.CursorStartPos.x+110,child->DC.CursorStartPos.y+2*(font+ImGui::GetStyle().ItemSpacing.y)+(font+2*ImGui::GetStyle().FramePadding.y)/2);
                        }
                    }
                    if(variant && frame==6) {
                        auto* combo=ImGui::FindWindowByName("##Combo_00");
                        if(combo && combo->Active) {
                            float font=g_app.editorFont->LegacySize;
                            pointer=ImVec2(combo->DC.CursorStartPos.x+70,combo->DC.CursorStartPos.y+(font+ImGui::GetStyle().ItemSpacing.y)*1.5f);
                        } else { ++failures; printf("FAIL source selector opens\n"); }
                    }
                    ImGui::Render(); glViewport(0,0,width,height); glClear(GL_COLOR_BUFFER_BIT);
                    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
                    if(frame==(variant ? 6 : 3)) SaveFrame(output/(variant ? "completion-sources.bmp" : "completion-cin.bmp"),width,height);
                }
                if(variant && completion->m_compItems[completion->m_compIdx].sourceIndex!=1) { ++failures; printf("FAIL source selector switches header\n"); }
                completion->CloseCompletion(); pointer=ImVec2(-100,-100);
            }
        }
        // Actual framebuffer checks for continuous multi-line/blank-line selection.
        g_app.active=0;
        CodeEditor* sample=g_app.ActiveEditor();
        sample->SetText("#include <iostream>\n\nint main() {\n\tint 数量 = 3;\n\n\tif (数量 > 0) {\n\t\tstd::cout << \"hello\\n\";\n\t}\n\n\treturn 0;\n}\n");
        sample->RebuildLines(); sample->m_scrollY=0; sample->m_scrollX=0;
        sample->m_selAnchor=sample->m_text.find("int main");
        sample->m_cursor=sample->m_text.find("return 0;")+9;
        sample->m_selActive=true; sample->m_cursorMoved=true;
        g_app.settings.rainbowIndentLines=true; g_app.settings.rainbowIndentFill=true;
        for(int frame=0; frame<6; ++frame) {
            if(frame==4) { sample->m_selActive=false; sample->m_selAnchor=sample->m_cursor; }
            ImGui_ImplOpenGL3_NewFrame(); ImGui_ImplGlfw_NewFrame();
            io.AddMousePosEvent(-100,-100); ImGui::NewFrame();
            DrawMainMenuBar(); DrawWorkspaceToolbar(); DrawStatusBar(); DrawActivityBar(); DrawWorkspaceDock();
            DrawFileTreeWindow(); g_app.console.DrawWindow(&g_app.showConsole);
            DefaultBottomDock(); DrawProblemsWindow();
            DefaultBottomDock(); g_app.terminal.DrawWindow(&g_app.showTerminal,true);
            DrawEditorWindow();
            ImGui::Render(); glViewport(0,0,width,height); glClear(GL_COLOR_BUFFER_BIT);
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
            if(frame==3) {
                bool checked=false;
                for(size_t row=0;row<sample->m_lines.size();++row) {
                    size_t pos=sample->LineStart(row);
                    float y=sample->m_drawPos.y+sample->VisibleRowOfLine((int)row)*sample->LineH()-sample->m_scrollY+sample->LineH()/2;
                    if(pos<sample->SelBegin() || pos>=sample->SelEnd() || sample->LineEnd(row)!=pos ||
                       y<sample->m_drawPos.y || y>=sample->m_drawPos.y+sample->m_drawSize.y) continue;
                    unsigned char pixel[3]{};
                    glReadPixels((int)(sample->m_drawPos.x+sample->m_drawSize.x-25),height-1-(int)y,1,1,GL_RGB,GL_UNSIGNED_BYTE,pixel);
                    ImVec4 expected=ImGui::ColorConvertU32ToFloat4(g_app.colors.selection);
                    if(std::abs((int)pixel[0]-(int)(expected.x*255))>2 || std::abs((int)pixel[1]-(int)(expected.y*255))>2 ||
                       std::abs((int)pixel[2]-(int)(expected.z*255))>2) { ++failures; printf("FAIL selected blank line is not continuous\n"); }
                    checked=true; break;
                }
                if(!checked) { ++failures; printf("FAIL no selected blank line tested\n"); }
                SaveFrame(output/(scenario==0 ? "selection-dark.bmp" : scenario==1 ? "selection-light.bmp" : "selection-compact.bmp"),width,height);
            }
            if(frame==5) SaveFrame(output/(scenario==0 ? "rainbow-dark.bmp" : scenario==1 ? "rainbow-light.bmp" : "rainbow-compact.bmp"),width,height);
            glfwSwapBuffers(window);
        }
        // Exercise real input handling without injecting OS input.
        std::string longText; for(int n=0;n<100;++n) longText+="    int item"+std::to_string(n)+" = 1;\n";
        sample->SetText(longText); sample->RebuildLines(); sample->m_scrollY=0;
        sample->m_cursor=0; sample->m_cursorMoved=false;
        ImVec2 dragStart(sample->m_drawPos.x+sample->GutterW()+sample->CharW()*2,sample->m_drawPos.y+sample->LineH()/2);
        for(int frame=0;frame<11;++frame) {
            ImGui_ImplOpenGL3_NewFrame(); ImGui_ImplGlfw_NewFrame(); io.DeltaTime=1.0f/60;
            io.AddFocusEvent(true);
            io.AddMousePosEvent(dragStart.x,frame>=3 && frame<=9 ? sample->m_drawPos.y+sample->m_drawSize.y+80 : dragStart.y);
            if(frame==2) io.AddMouseButtonEvent(0,true);
            if(frame==9) io.AddMouseButtonEvent(0,false);
            ImGui::NewFrame(); DrawMainMenuBar(); DrawWorkspaceToolbar(); DrawStatusBar(); DrawActivityBar(); DrawWorkspaceDock();
            DrawFileTreeWindow(); g_app.console.DrawWindow(&g_app.showConsole);
            DefaultBottomDock(); DrawProblemsWindow();
            DefaultBottomDock(); g_app.terminal.DrawWindow(&g_app.showTerminal,true);
            DrawEditorWindow(); ImGui::Render();
        }
        if(!sample->m_selActive || sample->SelEnd()<=sample->SelBegin() || sample->m_scrollY<=sample->LineH()) {
            ++failures; printf("FAIL drag selection/autoscroll\n");
        }
        for(int mode=0;mode<2;++mode) {
            sample->SetText("alpha beta\nsecond row\nthird row\n"); sample->RebuildLines();
            sample->m_scrollY=0; sample->m_scrollX=0; sample->m_cursor=0; sample->m_selAnchor=0; sample->m_cursorMoved=false;
            ImVec2 point(sample->m_drawPos.x+(mode ? sample->GutterW()+sample->CharW()*3 : sample->GutterW()-5),
                         sample->m_drawPos.y+sample->LineH()*(mode ? 1.5f : 0.5f));
            for(int frame=0;frame<5;++frame) {
                ImGui_ImplOpenGL3_NewFrame(); ImGui_ImplGlfw_NewFrame(); io.DeltaTime=1.0f/60;
                io.AddFocusEvent(true); io.AddMousePosEvent(point.x,point.y);
                io.AddKeyEvent(ImGuiMod_Shift,mode && frame<4);
                if(frame==1) io.AddMouseButtonEvent(0,true);
                if(frame==2) io.AddMouseButtonEvent(0,false);
                ImGui::NewFrame(); DrawMainMenuBar(); DrawWorkspaceToolbar(); DrawStatusBar(); DrawActivityBar(); DrawWorkspaceDock();
                DrawFileTreeWindow(); g_app.console.DrawWindow(&g_app.showConsole);
            DefaultBottomDock(); DrawProblemsWindow();
            DefaultBottomDock(); g_app.terminal.DrawWindow(&g_app.showTerminal,true);
            DrawEditorWindow(); ImGui::Render();
                if(frame==3) {
                    bool ok=mode ? sample->SelBegin()==0 && sample->SelEnd()>sample->LineStart(1) : sample->GetSelectionText()=="alpha beta\n";
                    if(!ok) { ++failures; printf("FAIL selection mode %d\n",mode); }
                }
            }
        }
        g_app.settings.rainbowIndentLines=false; g_app.settings.rainbowIndentFill=false;
        g_app.showSettings = true;
        g_app.settings.useCmdTemplate = scenario == 2;
        for (int frame = 0; frame < 4; ++frame) {
            ImGui_ImplOpenGL3_NewFrame(); ImGui_ImplGlfw_NewFrame();
            io.AddMousePosEvent(-100, -100);
            ImGui::NewFrame();
            ImGui::SetNextWindowPos(ImVec2(12,12));
            if (frame > 0) ImGui::SetWindowSize("设置", ImVec2(scenario == 2 ? 360 : 620, height - 24));
            DrawSettingsWindow();
            if(frame==1) {
                auto* parent=ImGui::FindWindowByName("设置");
                for(auto* child:ImGui::GetCurrentContext()->Windows)
                    if(child->ParentWindow==parent) ImGui::SetScrollY(child,550);
            }
            ImGui::Render(); glViewport(0,0,width,height);
            glClear(GL_COLOR_BUFFER_BIT); ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
            if (frame == 3) {
                auto* settings = ImGui::FindWindowByName("设置");
                bool bodyFound = false;
                for (auto* child : ImGui::GetCurrentContext()->Windows) {
                    if (child->ParentWindow != settings) continue;
                    bodyFound = true;
                    if (child->ScrollMax.x > 1) { ++failures; printf("FAIL settings horizontal overflow scenario %d\n", scenario); }
                    if (child->Pos.y + child->Size.y >= settings->Pos.y + settings->Size.y - 30) {
                        ++failures; printf("FAIL settings footer overlap\n");
                    }
                }
                if (!bodyFound) { ++failures; printf("FAIL settings body unavailable\n"); }
                SaveFrame(output / (scenario == 0 ? "settings-dark.bmp" : scenario == 1 ? "settings-light.bmp" : "settings-compact.bmp"), width,height);
            }
            glfwSwapBuffers(window);
        }
        auto beforeCustom=g_app.settings;
        for(int category=0;category<8;++category){
            g_settingsSection=category;g_settingsSearch.clear();
            for(int frame=0;frame<3;++frame){
                ImGui_ImplOpenGL3_NewFrame();ImGui_ImplGlfw_NewFrame();ImGui::NewFrame();
                ImGui::SetNextWindowPos(ImVec2(8,8));ImGui::SetWindowSize("设置",ImVec2(scenario==2?360:820,height-16));
                DrawSettingsWindow();
                auto* settings=ImGui::FindWindowByName("设置");
                for(auto* child:ImGui::GetCurrentContext()->Windows)if(frame==2&&child->ParentWindow==settings&&child->ScrollMax.x>1){++failures;printf("FAIL custom settings horizontal overflow category %d scenario %d\n",category,scenario);}
                ImGui::Render();glViewport(0,0,width,height);glClear(GL_COLOR_BUFFER_BIT);ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
                if(frame==2&&(category==3||category==6||category==7))SaveFrame(output/("preferences-"+std::to_string(category)+"-"+std::to_string(scenario)+".bmp"),width,height);
            }
        }
        g_settingsSearch=englishPreview?"pairing":"自动配对";g_settingsSection=0;
        ImGui_ImplOpenGL3_NewFrame();ImGui_ImplGlfw_NewFrame();ImGui::NewFrame();DrawSettingsWindow();ImGui::Render();
        if(g_settingsSection!=6){++failures;printf("FAIL settings keyword navigation\n");}
        g_settingsSearch.clear();g_settingsSection=3;
        g_app.settings.uiFontSize=22;g_app.settings.uiDensity=2;
        g_app.LoadFonts();ImGui_ImplOpenGL3_DestroyDeviceObjects();g_app.ApplyTheme();
        for(int frame=0;frame<3;++frame){
            ImGui_ImplOpenGL3_NewFrame();ImGui_ImplGlfw_NewFrame();ImGui::NewFrame();
            ImGui::SetNextWindowPos(ImVec2(8,8));ImGui::SetWindowSize("设置",ImVec2(scenario==2?360:820,height-16));
            DrawSettingsWindow();
            auto* settings=ImGui::FindWindowByName("设置");
            for(auto* child:ImGui::GetCurrentContext()->Windows)if(frame==2&&child->ParentWindow==settings&&child->ScrollMax.x>1){++failures;printf("FAIL large-font settings overflow scenario %d\n",scenario);}
            ImGui::Render();glViewport(0,0,width,height);glClear(GL_COLOR_BUFFER_BIT);ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
            if(frame==2)SaveFrame(output/("preferences-large-font-"+std::to_string(scenario)+".bmp"),width,height);
        }
        if(std::abs(ImGui::GetStyle().FontSizeBase-22)>0.1f){++failures;printf("FAIL UI font preference not applied\\n");}
        g_app.settings=beforeCustom;g_app.LoadFonts();ImGui_ImplOpenGL3_DestroyDeviceObjects();g_app.ApplyTheme();g_settingsSection=0;
        g_app.settings.useCmdTemplate = false;
        g_app.showSettings = false;
        // Exercise native docking with actual ImGui mouse events: pull a bottom
        // tab into a floating window, then save/reload its location.
        if(scenario==0) {
            ImVec2 tabPoint(-100,-100);
            auto* problem=ImGui::FindWindowByName("问题");
            ImGuiID originalDock=problem->DockId;
            auto* bar=problem->DockNode ? problem->DockNode->TabBar : nullptr;
            if(bar) for(auto& tab:bar->Tabs) if(tab.ID==problem->TabId)
                tabPoint=ImVec2(bar->BarRect.Min.x+tab.Offset+tab.Width/2,bar->BarRect.Min.y+bar->BarRect.GetHeight()/2);
            for(int frame=0;frame<20;++frame) {
                ImGui_ImplOpenGL3_NewFrame(); ImGui_ImplGlfw_NewFrame();
                io.DeltaTime=1.0f/60; io.AddFocusEvent(true);
                io.AddMousePosEvent(frame<10 ? tabPoint.x : 600,frame<10 ? tabPoint.y : 42);
                if(frame==7) io.AddMouseButtonEvent(0,true);
                if(frame==17) io.AddMouseButtonEvent(0,false);
                ImGui::NewFrame(); DrawMainMenuBar(); DrawWorkspaceToolbar(); DrawStatusBar(); DrawActivityBar(); DrawWorkspaceDock();
                DrawFileTreeWindow(); g_app.console.DrawWindow(&g_app.showConsole);
                DefaultBottomDock(); DrawProblemsWindow();
                DefaultBottomDock(); g_app.terminal.DrawWindow(&g_app.showTerminal,true); DrawEditorWindow();
                if(frame==4) {
                    auto* liveBar=problem->DockNode ? problem->DockNode->TabBar : nullptr;
                    if(liveBar) for(auto& tab:liveBar->Tabs) if(tab.ID==problem->TabId)
                        tabPoint=ImVec2(liveBar->BarRect.Min.x+tab.Offset+tab.Width/2,liveBar->BarRect.Min.y+liveBar->BarRect.GetHeight()/2);
                }
                ImGui::Render();
            }
            problem=ImGui::FindWindowByName("问题");
            if(problem->DockId || problem->Pos.y>150) { ++failures; printf("FAIL native drag undocks bottom panel: dock=%u y=%g\n",problem->DockId,problem->Pos.y); }
            size_t iniSize=0; std::string layout=ImGui::SaveIniSettingsToMemory(&iniSize);
            ImGui::LoadIniSettingsFromMemory(layout.c_str(),layout.size());
            auto* saved=ImGui::FindWindowSettingsByID(problem->ID);
            if(!saved || saved->DockId!=0) { ++failures; printf("FAIL dragged panel layout persistence\n"); }
            ImGui::DockBuilderDockWindow("问题",originalDock);
            // Show the redesigned debugger in its default bottom dock.
            g_app.showDebug=true;
            for(int frame=0;frame<4;++frame) {
                ImGui_ImplOpenGL3_NewFrame(); ImGui_ImplGlfw_NewFrame();
                io.AddMousePosEvent(-100,-100); ImGui::NewFrame();
                DrawMainMenuBar(); DrawWorkspaceToolbar(); DrawStatusBar(); DrawActivityBar(); DrawWorkspaceDock();
                DrawFileTreeWindow(); g_app.console.DrawWindow(&g_app.showConsole);
                DefaultBottomDock(); DrawProblemsWindow();
                DefaultBottomDock(); g_app.terminal.DrawWindow(&g_app.showTerminal,true);
                DefaultBottomDock(); DrawDebuggerWindow(); DrawEditorWindow();
                if(frame==1) ImGui::SetWindowFocus("调试");
                ImGui::Render(); glViewport(0,0,width,height); glClear(GL_COLOR_BUFFER_BIT);
                ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
                if(frame==3) SaveFrame(output/"debugger-dark.bmp",width,height);
            }
            g_app.showDebug=false;
        }
        if(scenario==0) {
            auto ai=std::filesystem::path(CCRIDE_TEST_PLUGIN_ROOT)/"ai-assistant";
            std::filesystem::create_directories(testPluginRoot/"ai-assistant");
            for(const char* name:{"plugin.json","ai-assistant.dll"}) std::filesystem::copy_file(ai/name,testPluginRoot/"ai-assistant"/name,std::filesystem::copy_options::overwrite_existing);
            g_app.plugins.ScanAndLoad();
            for(int frame=0;frame<5;++frame) {
                ImGui_ImplOpenGL3_NewFrame();ImGui_ImplGlfw_NewFrame();io.AddMousePosEvent(-100,-100);ImGui::NewFrame();
                DrawMainMenuBar();DrawWorkspaceToolbar();DrawStatusBar();DrawActivityBar();DrawWorkspaceDock();
                DrawFileTreeWindow();g_app.console.DrawWindow(&g_app.showConsole);DrawEditorWindow();g_app.plugins.CallFrames();
                ImGui::Render();glViewport(0,0,width,height);glClear(GL_COLOR_BUFFER_BIT);ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
                if(frame==4) {
                    auto* aiWindow=ImGui::FindWindowByName("AI 助手");auto* editorWindow=ImGui::FindWindowByName("编辑器");
                    if(!aiWindow || !aiWindow->DockId || aiWindow->Pos.x<=editorWindow->Pos.x) {++failures;printf("FAIL independent AI plugin tool window\n");}
                    SaveFrame(output/"ai-plugin.bmp",width,height);
                }
            }
        }
        if(scenario==0) {
            g_app.compilerCandidates={L"C:\\Toolchains\\GCC-11\\bin\\g++.exe",L"D:\\Toolchains\\GCC-14\\bin\\g++.exe"};
            g_app.compilerScanMessage="找到 2 个 g++，请选择要使用的编译器。";
            g_app.compilerPickerOpen=true;
            for(int frame=0;frame<3;++frame) {
                ImGui_ImplOpenGL3_NewFrame();ImGui_ImplGlfw_NewFrame();ImGui::NewFrame();
                g_app.DrawCompilerPicker();
                ImGui::Render();glViewport(0,0,width,height);glClear(GL_COLOR_BUFFER_BIT);ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
                if(frame==2) {
                    auto* picker=ImGui::FindWindowByName("选择 C++ 编译器");
                    if(!picker || picker->Size.x>width || picker->Size.y>height){++failures;printf("FAIL compiler picker layout\n");}
                    SaveFrame(output/"compiler-picker.bmp",width,height);
                }
            }
            g_app.compilerPickerOpen=false;g_app.compilerCandidates.clear();
        }
        if(scenario==0 && argc==3) {
            namespace fs=std::filesystem;
            fs::create_directories(testPluginRoot/"clangd");
            for(const char* name:{"plugin.json","clangd.dll"})fs::copy_file(fs::path(CCRIDE_TEST_PLUGIN_ROOT)/"clangd"/name,testPluginRoot/"clangd"/name,fs::copy_options::overwrite_existing);
            fs::create_directories(fs::path(g_app.dataDir)/"plugins/clangd");
            std::ofstream(fs::path(g_app.dataDir)/"plugins/clangd/settings.json")<<nlohmann::json{{"executable",argv[2]}}.dump();
            g_app.compilerCandidates.clear();g_app.compilerPickerOpen=false;
            auto* ed=g_app.editors.front().get();g_app.active=0;
            ed->SetPath((testPluginRoot/"nested-template.cpp").wstring());
            ed->SetText("#include <vector>\n\nusing Matrix = std::vector<std::vector<int>>;\n\nint main() {\n    Matrix matrix;\n    matrix.\n}\n");
            ed->RebuildLines();ed->m_cursor=ed->m_text.find("matrix.")+7;ed->m_selAnchor=ed->m_cursor;ed->m_cursorMoved=true;
            utils::WriteFileU8(ed->m_path,ed->m_text);g_app.plugins.ReloadAll();
            bool asked=false,found=false;int readyFrames=0;auto start=GetTickCount64();
            while(GetTickCount64()-start<15000) {
                ImGui_ImplOpenGL3_NewFrame();ImGui_ImplGlfw_NewFrame();io.AddFocusEvent(true);io.AddMousePosEvent(-100,-100);ImGui::NewFrame();
                DrawMainMenuBar();DrawWorkspaceToolbar();DrawStatusBar();DrawActivityBar();DrawWorkspaceDock();
                DrawFileTreeWindow();g_app.console.DrawWindow(&g_app.showConsole);DrawEditorWindow();g_app.plugins.CallFrames();
                if(language::Ready()&&!asked){ed->OpenCompletion();asked=true;}
                for(size_t i=0;i<ed->m_compItems.size();++i)if(ed->m_compItems[i].languageRevision&&ed->m_compItems[i].insertion=="push_back"){ed->m_compIdx=int(i);found=true;}
                ImGui::Render();glViewport(0,0,width,height);glClear(GL_COLOR_BUFFER_BIT);ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
                if(found&&++readyFrames==4){SaveFrame(output/"clangd-completion.bmp",width,height);break;}
                glfwSwapBuffers(window);Sleep(15);
            }
            if(!found){++failures;printf("FAIL real clangd completion preview\n");}
        }
        if(scenario==0 || scenario==2){
            auto* ed=g_app.ActiveEditor();ed->CloseCompletion();ed->SetText("abcd\nabcd\nabcd\n");ed->RebuildLines();ed->m_scrollY=ed->m_scrollX=0;ed->ColumnSelect(ed->CharW(),0,ed->CharW()*3,ed->LineH()*2.5f);
            for(int frame=0;frame<3;++frame){
                ImGui_ImplOpenGL3_NewFrame();ImGui_ImplGlfw_NewFrame();io.AddMousePosEvent(-100,-100);ImGui::NewFrame();
                DrawMainMenuBar();DrawWorkspaceToolbar();DrawStatusBar();DrawActivityBar();DrawWorkspaceDock();DrawEditorWindow();
                ImGui::Render();glViewport(0,0,width,height);glClear(GL_COLOR_BUFFER_BIT);ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());if(frame==2)SaveFrame(output/(scenario==0?"column-dark.bmp":"column-compact.bmp"),width,height);
            }
            ed->MultiReplace("");if(ed->m_text!="ad\nad\nad\n"){++failures;printf("FAIL column editing\n");}ed->Undo();
            bool open=true;
            for(int frame=0;frame<3;++frame){
                ImGui_ImplOpenGL3_NewFrame();ImGui_ImplGlfw_NewFrame();ImGui::NewFrame();
                ImGui::SetNextWindowPos(ImVec2(4,4));ImGui::SetNextWindowSize(ImVec2(width-8,height-8));contest::Draw(&open);
                ImGui::Render();glViewport(0,0,width,height);glClear(GL_COLOR_BUFFER_BIT);ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());if(frame==2)SaveFrame(output/(scenario==0?"tests-dark.bmp":"tests-compact.bmp"),width,height);
            }
            if(scenario==0){
                recovery::Tab tab;tab.name="未保存.cpp";tab.text="int main(){}";tab.dirty=true;g_app.recoverSnapshot.tabs={tab,tab};g_app.recoverSnapshot.tabs[1].name="另一个标签.cpp";g_app.recoverSelection={1,1};g_app.recoverPending=true;
                for(int frame=0;frame<3;++frame){ImGui_ImplOpenGL3_NewFrame();ImGui_ImplGlfw_NewFrame();ImGui::NewFrame();DrawRecoverModal();ImGui::Render();glViewport(0,0,width,height);glClear(GL_COLOR_BUFFER_BIT);ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());if(frame==2)SaveFrame(output/"all-tabs-recovery.bmp",width,height);}
                g_app.recoverSnapshot.tabs.clear();g_app.recoverSelection.clear();g_app.recoverPending=false;
            }
        }
        {
            bool tasksOpen=true;
            for(int frame=0;frame<3;++frame){
                ImGui_ImplOpenGL3_NewFrame();ImGui_ImplGlfw_NewFrame();ImGui::NewFrame();
                ImGui::SetNextWindowPos(ImVec2(4,4));ImGui::SetNextWindowSize(ImVec2(width-8,height-8));
                execution::DrawTaskWindow(&tasksOpen);
                ImGui::Render();glViewport(0,0,width,height);glClear(GL_COLOR_BUFFER_BIT);ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
                if(frame==2)SaveFrame(output/(scenario==2?"tasks-compact.bmp":scenario==1?"tasks-light.bmp":"tasks-dark.bmp"),width,height);
            }
        }

        if(scenario==0){
            g_app.plugins.UnloadAll();
            background::Index::I().Enable();
            auto* asyncEditor=g_app.ActiveEditor();
            if(asyncEditor){
                g_app.settings.codeCompletion=true;
                asyncEditor->SetText("#include <iostream>\nint main(){ std::cin. }\n");
                asyncEditor->RebuildLines();asyncEditor->m_cursor=asyncEditor->m_text.find("cin.")+4;
                asyncEditor->OpenCompletion();
                bool initiallyPending=asyncEditor->m_headerPending;
                auto started=GetTickCount64();bool hasGet=false;int completedFrames=0;
                do {
                    ImGui_ImplOpenGL3_NewFrame();ImGui_ImplGlfw_NewFrame();ImGui::NewFrame();
                    DrawMainMenuBar();DrawWorkspaceToolbar();DrawStatusBar();DrawActivityBar();DrawWorkspaceDock();
                    DrawFileTreeWindow();g_app.console.DrawWindow(&g_app.showConsole);DrawEditorWindow();
                    ImGui::Render();glViewport(0,0,width,height);glClear(GL_COLOR_BUFFER_BIT);ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
                    for(const auto& item:asyncEditor->m_compItems)if(item.label=="get")hasGet=true;
                    auto* popup=ImGui::FindWindowByName("##completion");
                    if(hasGet&&!asyncEditor->m_headerPending&&asyncEditor->m_compOpen&&popup&&popup->Active&&!popup->Hidden)++completedFrames;
                    if(completedFrames>=4){SaveFrame(output/"background-completion.bmp",width,height);break;}
                    Sleep(10);
                }while(GetTickCount64()-started<10000);
                if(!initiallyPending||!hasGet||asyncEditor->m_headerPending||!asyncEditor->m_compOpen||completedFrames<4){++failures;printf("Background completion failed: initial=%d get=%d pending=%d\n",initiallyPending,hasGet,asyncEditor->m_headerPending);}
            }else ++failures;
            background::Index::I().Stop();
        }

        if(scenario==0) UnloadDocumentationTestPlugin();
        g_shellIcons.Clear();
        ImGui_ImplOpenGL3_Shutdown(); ImGui_ImplGlfw_Shutdown(); ImGui::DestroyContext();
        g_app.editorFont = nullptr; g_app.uiFont = nullptr;
        glfwDestroyWindow(window);
    }
    glfwTerminate();
    CoUninitialize();
    printf("Workspace preview: %d failures\n", failures);
    return failures ? 1 : 0;
}
