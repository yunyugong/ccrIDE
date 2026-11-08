#include "i18n.h"
#include "session.h"
#include "lsp_support.h"
#include "utils.h"
#include <filesystem>
#include <map>
namespace clangd_plugin {
namespace fs=std::filesystem;
void Session::Event(json event) {
    std::lock_guard<std::mutex> lock(mutex);
    // Coalesce diagnostics/status, retaining bounded latest-state notifications.
    for(auto it=events.begin();it!=events.end();)if(it->value("kind","")==event.value("kind","") && it->value("file","")==event.value("file",""))it=events.erase(it);else ++it;
    if(events.size()>=64)events.pop_front();events.push_back(std::move(event));
}
void Session::Stop(){cancel=true;if(worker.joinable())worker.join();std::lock_guard<std::mutex> lock(mutex);commands.clear();events.clear();}
void Session::Document(const std::string& file,const std::string& text,unsigned long long revision) {
    if(text.size()>2*1024*1024)return;std::lock_guard<std::mutex> lock(mutex);
    for(auto it=commands.begin();it!=commands.end();)if(it->value("kind","")=="sync" && it->value("file","")==file)it=commands.erase(it);else ++it;
    if(commands.size()<64)commands.push_back({{"kind","sync"},{"file",file},{"text",text},{"revision",revision}});
}
void Session::Close(const std::string& file){std::lock_guard<std::mutex> lock(mutex);if(commands.size()<64)commands.push_back({{"kind","close"},{"file",file}});}
void Session::Request(const std::string& kind,const std::string& file,unsigned long long revision,size_t offset) {
    std::lock_guard<std::mutex> lock(mutex);
    for(auto it=commands.begin();it!=commands.end();)if(it->value("kind","")==kind)it=commands.erase(it);else ++it;
    if(commands.size()<64)commands.push_back({{"kind",kind},{"file",file},{"revision",revision},{"offset",offset}});
}
std::vector<json> Session::Poll(){std::lock_guard<std::mutex> lock(mutex);std::vector<json> result;for(int i=0;i<12&&!events.empty();++i){result.push_back(std::move(events.front()));events.pop_front();}return result;}
void Session::Start(Config config) {
    Stop();cancel=false;Event({{"kind","status"},{"ready",false},{"message",i18n::Tr("正在启动 clangd…")}});
    worker=std::thread([this,config](){
        try {
            if(!fs::is_regular_file(config.executable)){Event({{"kind","status"},{"ready",false},{"message",i18n::Tr("未找到 clangd，请在插件设置中指定路径")}});return;}
            struct Doc {std::string text;unsigned long long revision=0,parsedRevision=0;};
            struct Pending {std::string kind,file,text;unsigned long long revision;size_t offset;ULONGLONG time;};
            std::map<std::string,Doc> docs;std::map<std::string,json> deferred;std::map<long long,Pending> pending;long long next=1;
            bool initialized=false;std::string wire;lsp::Framer framer;ULONGLONG started=GetTickCount64(),rateStart=started;size_t rateBytes=0;
            auto send=[&](json message){wire+=lsp::Frame(message);if(wire.size()>8*1024*1024)throw std::runtime_error("LSP send limit");};
            auto notify=[&](const std::string& method,json params){send({{"jsonrpc","2.0"},{"method",method},{"params",std::move(params)}});};
            auto failed=[&](const Pending& p){Event({{"kind",p.kind.rfind("rename:",0)==0?"rename":p.kind},{"file",p.file},{"revision",p.revision},{"offset",p.offset},{"failed",true}});};
            auto reply=[&](const json& message) {
                if(message.value("jsonrpc",std::string{})!="2.0")throw std::runtime_error("Bad JSON-RPC version");
                if(message.contains("method")) {
                    auto method=message.at("method").get<std::string>();
                    if(message.contains("id")) {
                        if(method=="window/workDoneProgress/create")send({{"jsonrpc","2.0"},{"id",message["id"]},{"result",nullptr}});
                        else send({{"jsonrpc","2.0"},{"id",message["id"]},{"error",{{"code",-32601},{"message","Client method unsupported"}}}});
                    } else if(method=="textDocument/publishDiagnostics") {
                        auto params=message.at("params");if(!params.contains("version")||params["version"].is_null())return;
                        auto file=lsp::FileKey(lsp::Path(params.at("uri").get<std::string>()));auto doc=docs.find(file);if(doc==docs.end() || params["version"].get<unsigned long long>()!=doc->second.revision)return;
                        doc->second.parsedRevision=doc->second.revision;
                        for(auto it=deferred.begin();it!=deferred.end();) {
                            if(it->second.value("file",std::string{})==file && it->second.at("revision").get<unsigned long long>()==doc->second.revision) {
                                std::lock_guard<std::mutex> lock(mutex);bool newer=false;
                                for(const auto& queued:commands)if(queued.value("kind",std::string{})==it->first || (queued.value("file",std::string{})==file && queued.value("kind",std::string{})=="sync"))newer=true;
                                if(!newer && commands.size()<64)commands.push_back(it->second);
                                it=deferred.erase(it);
                            } else ++it;
                        }
                        json items=json::array();for(const auto& d:params.at("diagnostics")){if(items.size()>=300)break;if(d.value("severity",1)>2)continue;items.push_back({{"position",d.at("range").at("start")},{"error",d.value("severity",1)==1},{"message",d.at("message").get<std::string>().substr(0,2048)}});}
                        Event({{"kind","diagnostics"},{"file",file},{"revision",doc->second.revision},{"items",std::move(items)}});
                    }
                    return;
                }
                if(!message.contains("id") || !message["id"].is_number_integer())return;
                auto id=message["id"].get<long long>();
                if(id==1) {
                    if(message.contains("error") || !message.contains("result"))throw std::runtime_error("Initialize failed");
                    if(message["result"].value("capabilities",json::object()).value("positionEncoding",std::string("utf-16"))!="utf-16")throw std::runtime_error("Unsupported position encoding");
                    initialized=true;notify("initialized",json::object());Event({{"kind","status"},{"ready",true},{"message",i18n::Tr("clangd 已连接")}});return;
                }
                auto it=pending.find(id);if(it==pending.end())return;auto p=std::move(it->second);pending.erase(it);
                auto current=docs.find(p.file);if(current==docs.end()||current->second.revision!=p.revision)return;
                if(message.contains("error")){failed(p);return;}
                auto result=message.value("result",json());
                if(p.kind=="completion") {
                    json items=json::array(),list=result.is_null()?json::array():(result.is_array()?result:result.value("items",json::array()));
                    for(const auto& item:list) {
                        if(items.size()>=120)break;
                        if(item.value("insertTextFormat",1)==2 || (item.contains("additionalTextEdits")&&!item["additionalTextEdits"].empty()))continue;
                        std::string label=item.at("label").get<std::string>(),text=item.value("insertText",label);
                        auto first=label.find_first_not_of(" \t");if(first!=std::string::npos)label.erase(0,first);
                        size_t begin=p.offset,end=p.offset;
                        while(begin>0 && (std::isalnum(static_cast<unsigned char>(p.text[begin-1]))||p.text[begin-1]=='_'))--begin;
                        if(item.contains("textEdit")) {
                            auto edit=item["textEdit"];auto range=edit.contains("range")?edit["range"]:edit.at("replace");
                            begin=lsp::Offset(p.text,range.at("start"));end=lsp::Offset(p.text,range.at("end"));text=edit.at("newText").get<std::string>();
                        }
                        if(begin>end || text.size()>65536 || label.size()>2048)continue;
                        int symbolKind=item.value("kind",0);
                        items.push_back({{"label",label},{"text",text},{"detail",item.value("detail",std::string{}).substr(0,4096)},{"symbolKind",symbolKind},{"callable",symbolKind==2||symbolKind==3||symbolKind==4},{"start",begin},{"end",end}});
                    }
                    Event({{"kind",p.kind},{"file",p.file},{"revision",p.revision},{"offset",p.offset},{"items",std::move(items)}});
                } else if(p.kind.rfind("rename:",0)==0) {
                    json edits=json::array();
                    auto append=[&](const std::string& uri,const json& changes){auto file=lsp::Path(uri);for(const auto& change:changes){if(edits.size()>=5000)throw std::runtime_error("Rename edit limit");edits.push_back({{"file",file},{"range",change.at("range")},{"text",change.at("newText")}});}};
                    if(!result.is_null()){
                        if(result.contains("changes"))for(auto it=result["changes"].begin();it!=result["changes"].end();++it)append(it.key(),it.value());
                        if(result.contains("documentChanges"))for(const auto& change:result["documentChanges"]){if(!change.contains("textDocument"))throw std::runtime_error("File operations unsupported");append(change.at("textDocument").at("uri"),change.at("edits"));}
                    }
                    Event({{"kind","rename"},{"file",p.file},{"revision",p.revision},{"offset",p.offset},{"edits",std::move(edits)}});
                } else {
                    json location=result.is_array()?(result.empty()?json():result[0]):result;
                    if(location.is_null()){failed(p);return;}
                    auto uri=location.contains("uri")?location.at("uri"):location.at("targetUri");
                    auto range=location.contains("range")?location.at("range"):location.at("targetSelectionRange");
                    Event({{"kind",p.kind},{"file",p.file},{"revision",p.revision},{"offset",p.offset},{"target",lsp::Path(uri.get<std::string>())},{"position",range.at("start")}});
                }
            };
            json capabilities={{"general",{{"positionEncodings",json::array({"utf-16"})}}},{"textDocument",{{"completion",{{"completionItem",{{"snippetSupport",false},{"insertReplaceSupport",false}}}}},{"publishDiagnostics",{{"versionSupport",true}}}}}};
            send({{"jsonrpc","2.0"},{"id",1},{"method","initialize"},{"params",{{"processId",GetCurrentProcessId()},{"rootUri",config.database.empty()?json():json(lsp::Uri(utils::WToU8(config.database)))},{"capabilities",capabilities},{"initializationOptions",{{"fallbackFlags",config.flags}}}}}});
            execution::Request request;request.lane=execution::Lane::Service;request.label="clangd";request.logOutput=false;request.onStarted=[&](DWORD){started=GetTickCount64();};request.exe=config.executable;request.cwd=fs::path(config.executable).parent_path().wstring();
            request.args={L"--log=error",L"--background-index=0",L"--clang-tidy=0",L"--header-insertion=never",L"--completion-style=detailed",L"--completion-parse=always",L"--limit-results=120",L"--pch-storage=memory"};
            if(config.backgroundIndex)request.args[1]=L"--background-index=1";
            if(!config.database.empty())request.args.push_back(L"--compile-commands-dir="+config.database);
            if(!config.compiler.empty()){auto compiler=utils::WToU8(config.compiler);std::replace(compiler.begin(),compiler.end(),'\\','/');request.args.push_back(L"--query-driver="+utils::U8ToW(compiler));}
            // Driver probes may spawn cc1plus, which needs the selected toolchain DLLs.
            // Keep this local to clangd and its descendants; the IDE environment is unchanged.
            if(!config.compiler.empty())request.environment[L"PATH"]=utils::PathWithExecutableDirectory(config.compiler);
            request.capture=false;request.interactive=true;request.cancel=&cancel;request.limits.timeMs=0;request.limits.memoryBytes=config.memoryMiB*1024*1024;request.limits.outputBytes=1024ull*1024*1024;
            request.onOutput=[&](const std::string& bytes,bool errorChannel){
                try {auto now=GetTickCount64();if(now-rateStart>=1000){rateStart=now;rateBytes=0;}rateBytes+=bytes.size();if(rateBytes>16*1024*1024)throw std::runtime_error("LSP rate limit");if(!errorChannel)framer.Feed(bytes,reply);}
                catch(...){cancel=true;Event({{"kind","status"},{"ready",false},{"message",i18n::Tr("clangd 返回无效或超限消息，已停止，可手动重启")}});}
            };
            request.pollInput=[&](){
                try {
                    if(!initialized && GetTickCount64()-started>10000){cancel=true;return std::string{};}
                    if(initialized) {
                        std::deque<json> batch;{std::lock_guard<std::mutex> lock(mutex);batch.swap(commands);}
                        for(auto& command:batch) {
                            auto kind=command.at("kind").get<std::string>(),file=lsp::FileKey(command.at("file").get<std::string>());auto uri=lsp::Uri(file);
                            if(kind=="close") {if(docs.erase(file))notify("textDocument/didClose",{{"textDocument",{{"uri",uri}}}});continue;}
                            if(kind=="sync") {
                                bool opened=docs.count(file)!=0;auto& doc=docs[file];doc={command.at("text").get<std::string>(),command.at("revision").get<unsigned long long>()};
                                if(!opened) {
                                    bool database=!config.database.empty();auto parent=fs::u8path(file).parent_path();
                                    for(auto dir=parent;!database&&!dir.empty();dir=dir.parent_path()){if(fs::is_regular_file(dir/"compile_commands.json")||fs::is_regular_file(dir/"build/compile_commands.json"))database=true;if(dir==dir.parent_path())break;}
                                    if(!database&&!config.compiler.empty()) {
                                        std::vector<std::string> argv{utils::WToU8(config.compiler)};argv.insert(argv.end(),config.flags.begin(),config.flags.end());argv.push_back(file);
                                        json changes=json::object();changes[file]={{"workingDirectory",utils::WToU8(parent.wstring())},{"compilationCommand",argv}};
                                        notify("workspace/didChangeConfiguration",{{"settings",{{"compilationDatabaseChanges",changes}}}});
                                    }
                                    notify("textDocument/didOpen",{{"textDocument",{{"uri",uri},{"languageId","cpp"},{"version",doc.revision},{"text",doc.text}}}});
                                } else notify("textDocument/didChange",{{"textDocument",{{"uri",uri},{"version",doc.revision}}},{"contentChanges",json::array({{{"text",doc.text}}})},{"wantDiagnostics",true}});
                                continue;
                            }
                            auto doc=docs.find(file);if(doc==docs.end() || doc->second.revision!=command.at("revision").get<unsigned long long>())continue;
                            if(doc->second.parsedRevision!=doc->second.revision) {
                                command["queuedAt"]=GetTickCount64();deferred[kind]=command;continue;
                            }
                            deferred.erase(kind);
                            for(auto it=pending.begin();it!=pending.end();)if(it->second.kind==kind){notify("$/cancelRequest",{{"id",it->first}});it=pending.erase(it);}else ++it;
                            auto id=++next;size_t offset=command.at("offset").get<size_t>();pending[id]={kind,file,doc->second.text,doc->second.revision,offset,GetTickCount64()};
                            json params={{"textDocument",{{"uri",uri}}},{"position",lsp::Position(doc->second.text,offset)}};
                            bool rename=kind.rfind("rename:",0)==0;if(rename)params["newName"]=kind.substr(7);
                            send({{"jsonrpc","2.0"},{"id",id},{"method",rename?"textDocument/rename":kind=="completion"?"textDocument/completion":"textDocument/definition"},{"params",std::move(params)}});
                        }
                    }
                    for(auto it=deferred.begin();it!=deferred.end();)if(GetTickCount64()-it->second.value("queuedAt",0ull)>8000) {
                        auto& command=it->second;
                        failed(Pending{it->first,command.at("file").get<std::string>(),{},command.at("revision").get<unsigned long long>(),command.at("offset").get<size_t>(),0});it=deferred.erase(it);
                    }else ++it;
                    for(auto it=pending.begin();it!=pending.end();)if(GetTickCount64()-it->second.time>8000){notify("$/cancelRequest",{{"id",it->first}});failed(it->second);it=pending.erase(it);}else ++it;
                    auto result=std::move(wire);wire.clear();return result;
                }catch(...){cancel=true;return std::string{};}
            };
            auto result=execution::Run(request);
            for(const auto& p:pending)failed(p.second);
            Event({{"kind","status"},{"ready",false},{"message",std::string(i18n::Tr("clangd 已停止，可在插件设置中重启: "))+execution::ReasonText(result.reason)}});
        }catch(...) {Event({{"kind","status"},{"ready",false},{"message",i18n::Tr("clangd 启动或协议处理失败，请检查配置后重试")}});}
    });
}
}
