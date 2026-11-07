#include "language_host.h"
#include "app.h"
#include "lsp_support.h"
#include <map>
#include <set>
namespace language {
struct Document {std::string file,text;unsigned long long revision=0;};
struct Pending {unsigned long long uid=0,revision=0;size_t offset=0;ULONGLONG time=0;};
static PluginLanguageHooks hooks{};static bool ready=false,fallback=false;static unsigned long long version=0;
static std::map<unsigned long long,Document> documents;
static std::map<unsigned long long,Pending> completions;static Pending definition;
static Document* Sync(CodeEditor& editor) {
    if(!hooks.Document || editor.m_path.empty() || editor.m_text.size()>2*1024*1024)return nullptr;
    auto& doc=documents[editor.m_uid];auto file=lsp::FileKey(utils::WToU8(editor.m_path));
    if(doc.file!=file || doc.text!=editor.m_text) {
        if(!doc.file.empty() && doc.file!=file && hooks.Close)hooks.Close(doc.file.c_str());
        doc={file,editor.m_text,++version};hooks.Document(doc.file.c_str(),doc.text.data(),doc.text.size(),doc.revision);
    }
    return &doc;
}
bool Ready(){return ready && hooks.Request;}
bool Handles(const CodeEditor* ed){return Ready() && ed && !ed->m_path.empty() && ed->m_text.size()<=2*1024*1024 && documents.count(ed->m_uid)!=0;}
bool Current(const CodeEditor& ed,unsigned long long revision){auto it=documents.find(ed.m_uid);return it!=documents.end()&&it->second.revision==revision&&it->second.file==lsp::FileKey(utils::WToU8(ed.m_path))&&it->second.text==ed.m_text;}
static CodeEditor* Match(const Pending& p){for(auto& e:g_app.editors)if(e->m_uid==p.uid&&Current(*e,p.revision)&&e->m_cursor==p.offset)return e.get();return nullptr;}
static void FallbackCompletion(const Pending& p) {
    auto* e=Match(p);if(!e||!e->m_compOpen||!e->m_languagePending)return;
    e->m_languagePending=false;fallback=true;e->GatherCandidates(e->m_compPrefix,e->m_compItems);fallback=false;e->m_compOpen=!e->m_compItems.empty();
}
void Reset(){bool owned=hooks.Document!=nullptr;ready=false;hooks={};documents.clear();completions.clear();definition={};for(auto& e:g_app.editors){e->m_languagePending=false;e->CloseCompletion();if(owned)e->m_diags.clear();}if(owned)g_app.MarkDiag();}
void Register(const PluginLanguageHooks* value){Reset();if(value)hooks=*value;}
bool Complete(CodeEditor& ed) {
    if(!Ready()||fallback||!g_app.settings.codeCompletion)return false;
    size_t hs;bool quote;if(ed.InIncludeCompletion(hs,quote))return false;
    auto* doc=Sync(ed);if(!doc)return false;
    ed.m_languagePending=true;completions[ed.m_uid]={ed.m_uid,doc->revision,ed.m_cursor,GetTickCount64()};
    hooks.Request("completion",doc->file.c_str(),doc->revision,ed.m_cursor);return true;
}
bool Definition(CodeEditor& ed){if(!Ready())return false;auto* doc=Sync(ed);if(!doc)return false;definition={ed.m_uid,doc->revision,ed.m_cursor,GetTickCount64()};hooks.Request("definition",doc->file.c_str(),doc->revision,ed.m_cursor);return true;}
void Tick() {
    if(!hooks.Document)return;
    static ULONGLONG last=0;if(GetTickCount64()-last<150)return;last=GetTickCount64();
    std::set<unsigned long long> live;size_t total=0;
    for(auto& ed:g_app.editors){total+=ed->m_text.size();if(live.size()<32 && total<=16*1024*1024){live.insert(ed->m_uid);Sync(*ed);}}
    for(auto it=documents.begin();it!=documents.end();)if(!live.count(it->first)){if(hooks.Close)hooks.Close(it->second.file.c_str());it=documents.erase(it);}else ++it;
    for(auto it=completions.begin();it!=completions.end();)if(GetTickCount64()-it->second.time>10000){FallbackCompletion(it->second);it=completions.erase(it);}else ++it;
    if(definition.uid&&GetTickCount64()-definition.time>10000){auto p=definition;definition={};if(auto* e=Match(p))e->GotoDefinition(false);}
}
void Publish(const char* text,size_t length) {
    if(!text || length>2*1024*1024)return;
    try {
        auto message=lsp::Parse(std::string(text,length));auto kind=message.value("kind",std::string{});
        if(kind=="status") {
            bool next=message.value("ready",false);
            if(ready&&!next){for(auto& entry:completions)FallbackCompletion(entry.second);completions.clear();if(auto* e=Match(definition))e->GotoDefinition(false);definition={};for(auto& ed:g_app.editors)ed->m_diags.clear();g_app.MarkDiag();}
            ready=next;return;
        }
        auto file=message.at("file").get<std::string>();auto rev=message.at("revision").get<unsigned long long>();
        CodeEditor* ed=nullptr;for(auto& e:g_app.editors)if(lsp::FileKey(utils::WToU8(e->m_path))==lsp::FileKey(file)&&Current(*e,rev)){ed=e.get();break;}if(!ed)return;
        if(kind=="completion") {
            auto it=completions.find(ed->m_uid);if(it==completions.end()||it->second.offset!=message.at("offset").get<size_t>()||!Match(it->second))return;
            auto pending=it->second;completions.erase(it);
            if(message.value("failed",false)){FallbackCompletion(pending);return;}
            if(!ed->m_compOpen||!ed->m_languagePending)return;ed->m_languagePending=false;ed->m_compItems.clear();
            for(const auto& item:message.at("items")) {
                if(ed->m_compItems.size()>=120)break;CodeEditor::CompItem result;
                result.label=item.at("label").get<std::string>();result.hint=item.value("detail",std::string{});
                result.insertion=item.at("text").get<std::string>();result.replaceStart=item.at("start").get<size_t>();result.replaceEnd=item.at("end").get<size_t>();
                if(result.replaceStart>result.replaceEnd||result.replaceEnd>ed->m_text.size()||result.insertion.size()>65536||result.insertion.find('\0')!=std::string::npos)continue;
                auto boundary=[&](size_t at){return at==ed->m_text.size() || (static_cast<unsigned char>(ed->m_text[at])&0xc0)!=0x80;};
                if(!boundary(result.replaceStart)||!boundary(result.replaceEnd))continue;
                int k=item.value("symbolKind",0);result.kind=k==9?"命名空间":((k==7||k==22||k==25)?"类型":((k==2||k==3||k==4)?"函数":((k==5||k==6)?"变量":(k==14?"关键字":"语义"))));
                result.func=item.value("callable",false);result.languageRevision=rev;result.languageOffset=message.at("offset").get<size_t>();ed->m_compItems.push_back(std::move(result));
            }
            ed->m_compIdx=0;ed->m_compScroll=0;ed->m_compOpen=!ed->m_compItems.empty();
        } else if(kind=="diagnostics" && g_app.settings.autoCheck) {
            std::vector<Diag> diags;for(const auto& item:message.at("items")){if(diags.size()>=300)break;auto pos=item.at("position");size_t at=lsp::Offset(ed->m_text,pos);auto line=ed->LineOf(at);diags.push_back({int(line+1),int(at-ed->LineStart(line)+1),item.value("error",true),item.at("message").get<std::string>()});}ed->m_diags=std::move(diags);g_app.SyncDiagPanel();
        } else if(kind=="definition") {
            auto p=definition;if(!p.uid||p.offset!=message.at("offset").get<size_t>()||Match(p)!=ed)return;definition={};
            if(message.value("failed",false)||!message.contains("target")){ed->GotoDefinition(false);return;}
            auto target=utils::U8ToW(message.at("target").get<std::string>());int row=message.at("position").at("line").get<int>();
            for(auto& open:g_app.editors)if(lsp::FileKey(utils::WToU8(open->m_path))==lsp::FileKey(utils::WToU8(target))){target=open->m_path;break;}
            g_app.OpenAtFile(utils::WToU8(target),row+1);if(auto* opened=g_app.ActiveEditor()){opened->m_cursor=lsp::Offset(opened->m_text,message.at("position"));opened->m_selAnchor=opened->m_cursor;opened->m_cursorMoved=true;}
        }
    }catch(...) {/* Malformed/stale responses cannot mutate the editor. */}
}
}
