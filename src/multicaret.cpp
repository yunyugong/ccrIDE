#include "editor.h"
#include "app.h"
#include <algorithm>
#include <imgui_internal.h>
void CodeEditor::AddCaret(size_t offset){
 if(m_carets.empty())m_carets.push_back({m_selAnchor,m_cursor});
 for(auto& c:m_carets)if(c.cursor==offset&&c.anchor==offset)return;
 m_carets.push_back({offset,offset});m_cursor=offset;m_selAnchor=offset;m_selActive=false;CloseCompletion();CloseCalltip();
}
void CodeEditor::ColumnSelect(float x1,float y1,float x2,float y2){
 RebuildVisCache();m_carets.clear();int top=std::max(0,int(std::min(y1,y2)/LineH())),bottom=std::min(int(m_visLine.size())-1,int(std::max(y1,y2)/LineH()));
 for(int row=top;row<=bottom;++row){float y=(row+0.5f)*LineH();m_carets.push_back({HitTest(x1,y),HitTest(x2,y)});}
 if(!m_carets.empty()){m_cursor=m_carets.back().cursor;m_selAnchor=m_cursor;m_selActive=false;}CloseCompletion();CloseCalltip();
}
void CodeEditor::MultiReplace(const std::string& text,int deletion,bool distribute){
 if(m_carets.empty())return;
 struct Edit{size_t a,b;std::string text;};std::vector<Edit> edits;
 std::sort(m_carets.begin(),m_carets.end(),[](auto a,auto b){return std::min(a.anchor,a.cursor)<std::min(b.anchor,b.cursor);});
 std::vector<std::string> lines;size_t at=0;
 if(distribute){while(at<=text.size()){auto nl=text.find('\n',at);auto line=text.substr(at,nl-at);if(!line.empty()&&line.back()=='\r')line.pop_back();lines.push_back(line);if(nl==std::string::npos)break;at=nl+1;}}
 for(size_t i=0;i<m_carets.size();++i){auto c=m_carets[i];size_t a=std::min(c.anchor,c.cursor),b=std::max(c.anchor,c.cursor);if(a>m_text.size()||b>m_text.size())continue;
 if(a==b){if(deletion<0)a=PrevChar(a);if(deletion>0)b=NextChar(b);}
 if(!edits.empty()&&a<edits.back().b){edits.back().b=std::max(edits.back().b,b);continue;}
 if(!edits.empty()&&a==edits.back().a&&b==edits.back().b)continue;
 std::string insertion=distribute&&lines.size()==m_carets.size()?lines[i]:text;
 if(!distribute&&text=="\n"&&g_app.settings.autoIndent){
  auto prefix=m_text.substr(LineStart(LineOf(a)),a-LineStart(LineOf(a)));
  size_t n=prefix.find_first_not_of(" \t");if(n==std::string::npos)n=prefix.size();
  std::string indent=prefix.substr(0,n);
  if(g_app.settings.insertSpaces){size_t columns=0;for(char c:indent)columns=c=='\t'?(columns/TabWidth()+1)*TabWidth():columns+1;indent.assign(columns,' ');}
  if(!prefix.empty()&&prefix.back()=='{')indent+=IndentUnit();
  insertion+=indent;
 }
 edits.push_back({a,b,insertion});
 }
 std::string before=m_text;for(auto i=edits.rbegin();i!=edits.rend();++i){EraseRange(i->a,i->b,false);InsertAt(i->a,i->text,false);}
 if(before!=m_text)PushUndo(0,before,m_text);
 m_carets.clear();long long delta=0;for(auto& e:edits){size_t pos=size_t((long long)e.a+delta)+e.text.size();m_carets.push_back({pos,pos});delta+=(long long)e.text.size()-(long long)(e.b-e.a);}
 if(!m_carets.empty())m_cursor=m_selAnchor=m_carets.back().cursor;m_selActive=false;m_cursorMoved=true;CloseCompletion();
}
void CodeEditor::MultiIndent(bool newline){
 if(newline){MultiReplace("\n");return;}
 std::sort(m_carets.begin(),m_carets.end(),[](auto a,auto b){return std::min(a.anchor,a.cursor)<std::min(b.anchor,b.cursor);});
 std::string all;
 for(size_t i=0;i<m_carets.size();++i){
  if(i)all+='\n';size_t pos=std::min(m_carets[i].anchor,m_carets[i].cursor);
  all+=g_app.settings.insertSpaces?std::string(TabWidth()-VisualCol(pos)%TabWidth(),' '):"\t";
 }
 MultiReplace(all,0,true);
}
bool CodeEditor::MultiKeyboard(){
 if(m_carets.empty())return false;auto& io=ImGui::GetIO();
 if(ImGui::IsKeyPressed(ImGuiKey_Escape)){m_carets.clear();return true;}
 if(io.KeyCtrl&&(ImGui::IsKeyPressed(ImGuiKey_Z)||ImGui::IsKeyPressed(ImGuiKey_Y))){bool undo=ImGui::IsKeyPressed(ImGuiKey_Z);m_carets.clear();if(undo)Undo();else Redo();return true;}
 if(io.KeyCtrl&&(ImGui::IsKeyPressed(ImGuiKey_C)||ImGui::IsKeyPressed(ImGuiKey_X))){std::string copy;for(auto& c:m_carets){if(!copy.empty())copy+='\n';copy+=m_text.substr(std::min(c.anchor,c.cursor),std::max(c.anchor,c.cursor)-std::min(c.anchor,c.cursor));}ImGui::SetClipboardText(copy.c_str());if(ImGui::IsKeyPressed(ImGuiKey_X))MultiReplace("");}
 if(io.KeyCtrl&&ImGui::IsKeyPressed(ImGuiKey_V)){auto text=ImGui::GetClipboardText();if(text){std::string normalized;for(char c:std::string(text)){if(c=='\r')continue;normalized+=c=='\t'?(g_app.settings.insertSpaces?std::string(TabWidth(),' '):"\t"):std::string(1,c);}MultiReplace(normalized,0,true);}}
 if(ImGui::IsKeyPressed(ImGuiKey_Backspace))MultiReplace("",-1);
 if(ImGui::IsKeyPressed(ImGuiKey_Delete))MultiReplace("",1);
 if(!io.KeyCtrl&&ImGui::IsKeyPressed(ImGuiKey_Tab))MultiIndent(false);
 if(ImGui::IsKeyPressed(ImGuiKey_Enter)||ImGui::IsKeyPressed(ImGuiKey_KeypadEnter))MultiIndent(true);
 for(auto key:{ImGuiKey_LeftArrow,ImGuiKey_RightArrow,ImGuiKey_UpArrow,ImGuiKey_DownArrow,ImGuiKey_Home,ImGuiKey_End})if(ImGui::IsKeyPressed(key))for(auto& c:m_carets){
  m_cursorMoved=true;
  size_t old=c.cursor;
  if(key==ImGuiKey_LeftArrow)c.cursor=io.KeyCtrl?WordLeft(old):PrevChar(old);
  if(key==ImGuiKey_RightArrow)c.cursor=io.KeyCtrl?WordRight(old):NextChar(old);
  if(key==ImGuiKey_Home)c.cursor=LineStart(LineOf(old));
  if(key==ImGuiKey_End)c.cursor=LineEnd(LineOf(old));
  if(key==ImGuiKey_UpArrow||key==ImGuiKey_DownArrow){int row=VisibleRowOfLine(int(LineOf(old)))+(key==ImGuiKey_UpArrow?-1:1);c.cursor=HitTest(CharX(old),(std::max(0,row)+0.5f)*LineH());}
  if(!io.KeyShift)c.anchor=c.cursor;
 }
 std::string text;for(unsigned int c:io.InputQueueCharacters)if(c>=32){char utf8[5]{};int n=ImTextCharToUtf8(utf8,c);text.append(utf8,n);}
 if(!text.empty()&&!io.KeyCtrl)MultiReplace(text);io.InputQueueCharacters.resize(0);
 if(!m_carets.empty())m_cursor=m_carets.back().cursor;m_selAnchor=m_cursor;m_selActive=false;return true;
}
