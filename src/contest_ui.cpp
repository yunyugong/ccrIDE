#include "i18n.h"
#include "contest.h"
#include "app.h"
#include "ui_helpers.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <shellapi.h>
namespace contest {
static Runner runner;static Config config;static int selected=0;static std::string message;static std::wstring loaded;
static bool Field(const char* label,std::string& value){ImGui::TextWrapped("%s",label);ImGui::SetNextItemWidth(-FLT_MIN);return ui::InputTextStd((std::string("##")+label).c_str(),value);}
static void Save(){
 try{nlohmann::json js={{"schema",1},{"compare",int(config.compare)},{"rounds",config.rounds},{"seed",config.seed},{"reference",utils::WToU8(config.reference)},{"generator",utils::WToU8(config.generator)},{"cases",nlohmann::json::array()},{"groups",nlohmann::json::array()}};
 for(auto& c:config.cases)js["cases"].push_back({{"name",c.name},{"input",c.input},{"expected",c.expected},{"group",c.group},{"enabled",c.enabled}});
 for(auto& g:config.groups)js["groups"].push_back({{"name",g.name},{"points",g.points},{"requires",g.requires}});
 if(!utils::WriteFileU8(g_app.dataDir+L"\\tests.json",js.dump(2)))throw std::runtime_error(i18n::Tr("无法保存测试配置"));message=i18n::Tr("配置已保存");
 }catch(const std::exception& e){message=e.what();}
}
static void Load(){
 loaded=g_app.dataDir;config=Config{};std::string text;
 try{if(utils::ReadFileU8(g_app.dataDir+L"\\tests.json",text)){auto js=nlohmann::json::parse(text);config.compare=js.value("compare",1)==0?Compare::Exact:Compare::Tokens;config.rounds=js.value("rounds",100);config.seed=js.value("seed",1ull);config.reference=utils::U8ToW(js.value("reference",""));config.generator=utils::U8ToW(js.value("generator",""));for(auto& c:js.at("cases"))config.cases.push_back({c.at("name"),c.at("input"),c.at("expected"),c.value("group","default"),c.value("enabled",true)});for(auto& g:js.at("groups"))config.groups.push_back({g.at("name"),g.at("points"),g.value("requires",std::vector<std::string>{})});}}
 catch(...){message=i18n::Tr("测试配置损坏，原文件保留");}if(config.groups.empty())config.groups.push_back({});if(config.cases.empty())config.cases.push_back({i18n::Tr("样例 1"),"",""});
}
void Shutdown(){runner.Stop();}
void Draw(bool* open){
 if(loaded!=g_app.dataDir)Load();auto state=runner.Snapshot();
 ImGui::SetNextWindowSize(ImVec2(820,650),ImGuiCond_FirstUseEver);
 if(ImGui::Begin(i18n::Window("测试与对拍"),open)){
 ImGui::TextWrapped("%s",state.status.c_str());ImGui::TextWrapped("%s",message.c_str());
 if(state.running){if(ImGui::Button(i18n::Tr("停止测试")))runner.Cancel();}
 else {
  if(ImGui::Button(i18n::Tr("保存配置")))Save();ImGui::SameLine();if(ImGui::Button(i18n::Tr("打开归档"))&&!state.archive.empty())ShellExecuteW(nullptr,L"open",utils::U8ToW(state.archive).c_str(),nullptr,nullptr,SW_SHOWNORMAL);
 }
 ImGui::BeginDisabled(state.running);
 ImGui::TextWrapped(i18n::Tr("时间、内存和输出限制使用设置中的运行限制；启用隔离目录后，每次执行使用独立工作目录。"));
 static std::string importDirectory,replayDirectory;
 Field(i18n::Tr("导入目录（.in / 同名 .out）"),importDirectory);
 if(ImGui::Button(i18n::Tr("导入样例目录")))try{
  int added=0;for(auto& entry:std::filesystem::directory_iterator(std::filesystem::u8path(importDirectory)))if(entry.is_regular_file()&&entry.path().extension()==L".in"){
   if(config.cases.size()>=1000)throw std::runtime_error(i18n::Tr("样例上限 1000"));
   auto output=entry.path();output.replace_extension(L".out");Case c;c.name=utils::WToU8(entry.path().stem().wstring());
   if(!utils::ReadFileU8(entry.path().wstring(),c.input)||!utils::ReadFileU8(output.wstring(),c.expected))throw std::runtime_error(i18n::Tr("输入或同名 .out 不可读"));config.cases.push_back(std::move(c));++added;
  }message=i18n::Tr("导入 ")+std::to_string(added)+i18n::Tr(" 个样例");
 }catch(const std::exception& e){message=e.what();}
 Field(i18n::Tr("重放目录（归档内 case-N）"),replayDirectory);
 if(ImGui::Button(i18n::Tr("将归档输入导入为样例"))){Case c;c.name=i18n::Tr("归档重放");auto dir=std::filesystem::u8path(replayDirectory);if(utils::ReadFileU8((dir/"input.txt").wstring(),c.input)&&utils::ReadFileU8((dir/"expected.txt").wstring(),c.expected)){config.cases.push_back(std::move(c));message=i18n::Tr("已导入，可直接运行，无需重新生成随机数据");}else message=i18n::Tr("归档输入/期望输出不可读");}
 int comparison=int(config.compare);ImGui::RadioButton(i18n::Tr("严格字节比较"),&comparison,0);ImGui::SameLine();ImGui::RadioButton(i18n::Tr("忽略空白的 Token 比较"),&comparison,1);config.compare=Compare(comparison);
 auto start=[&](bool stress){
  auto* ed=g_app.ActiveEditor();if(!ed){message=i18n::Tr("请先打开待测源码");return;}
  if((!ed->HasPath()||ed->m_dirty)&&!g_app.SaveActive(false)){message=i18n::Tr("请保存待测源码");return;}
  if(g_app.settings.useCmdTemplate){message=i18n::Tr("测试使用结构化编译参数，请先关闭自定义编译命令");return;}
  config.solution=ed->m_path;config.compiler=utils::U8ToW(g_app.settings.gppPath);config.archive=g_app.dataDir+L"\\runs";config.stress=stress;config.isolatedWorkingDirectory=g_app.settings.isolatedRunDirectory;
  if(stress){for(auto* path:{&config.reference,&config.generator})if(!path->empty()&&std::filesystem::path(*path).is_relative())*path=(std::filesystem::path(config.solution).parent_path()/ *path).lexically_normal().wstring();}
  const wchar_t* standards[]={L"-std=c++17",L"-std=c++20",L"-std=c++14",L"-std=c++11",L"-std=c++23"};
  config.flags={standards[std::clamp(g_app.settings.stdVersion,0,4)]};
  const wchar_t* optimizations[]={L"",L"-O2",L"-O0",L"-O1",L"-O3",L"-Os"};
  auto optimization=optimizations[std::clamp(g_app.settings.optLevel,0,5)];if(*optimization)config.flags.push_back(optimization);
  if(g_app.settings.warnAll){config.flags.push_back(L"-Wall");config.flags.push_back(L"-Wextra");}if(g_app.settings.dbgFlag)config.flags.push_back(L"-g");
  int argc=0;auto args=CommandLineToArgvW((L"flags "+utils::U8ToW(g_app.settings.extraFlags)).c_str(),&argc);if(args){for(int i=1;i<argc;++i)config.flags.push_back(args[i]);LocalFree(args);}
  config.limits.timeMs=std::clamp(g_app.settings.runTimeLimitMs,1,3600000);config.limits.memoryBytes=size_t(std::max(0,g_app.settings.runMemoryLimitMB))*1024*1024;config.limits.outputBytes=size_t(std::clamp(g_app.settings.runOutputLimitMB,1,64))*1024*1024;
  config.rounds=std::clamp(config.rounds,1,10000);Save();runner.Start(config);
 };
 if(ImGui::BeginTabBar("##test_modes")){
 if(ImGui::BeginTabItem(i18n::Tr("样例 / Subtask"))){
  if(ImGui::Button(i18n::Tr("添加样例"))){config.cases.push_back({i18n::Tr("样例 ")+std::to_string(config.cases.size()+1),"",""});selected=int(config.cases.size()-1);}
  ImGui::SameLine();if(ImGui::Button(i18n::Tr("运行全部启用样例")))start(false);
  if(ImGui::BeginListBox("##test_cases",ImVec2(-1,110))){
  for(size_t i=0;i<config.cases.size();++i){ImGui::PushID(int(i));if(ImGui::Selectable(config.cases[i].name.c_str(),selected==int(i)))selected=int(i);ImGui::PopID();}
  ImGui::EndListBox();}
  if(!config.cases.empty()){selected=std::clamp(selected,0,int(config.cases.size()-1));auto& c=config.cases[selected];Field(i18n::Tr("名称"),c.name);Field("Subtask",c.group);ImGui::Checkbox(i18n::Tr("启用"),&c.enabled);ImGui::TextUnformatted(i18n::Tr("输入"));ui::InputTextMultilineStd("##case_input",c.input,ImVec2(-1,90));ImGui::TextUnformatted(i18n::Tr("期望输出"));ui::InputTextMultilineStd("##case_expected",c.expected,ImVec2(-1,90));
  if(ImGui::Button(i18n::Tr("删除该样例"))){config.cases.erase(config.cases.begin()+selected);selected=0;}}
  if(ImGui::TreeNode(i18n::Tr("Subtask 评分（组内全部 AC 才得分）"))){
   if(ImGui::Button(i18n::Tr("添加 Subtask")))config.groups.push_back({"group"+std::to_string(config.groups.size()+1),0,{}});
   for(size_t i=0;i<config.groups.size();++i){auto& g=config.groups[i];ImGui::PushID(int(i));Field(i18n::Tr("组名"),g.name);ImGui::InputDouble(i18n::Tr("分值"),&g.points);std::string deps;for(auto& d:g.requires){if(!deps.empty())deps+=",";deps+=d;}if(Field(i18n::Tr("依赖组（逗号分隔）"),deps)){g.requires.clear();size_t a=0;while(a<deps.size()){auto b=deps.find(',',a);g.requires.push_back(deps.substr(a,b-a));if(b==std::string::npos)break;a=b+1;}}ImGui::Separator();ImGui::PopID();}ImGui::TreePop();
  }
  ImGui::EndTabItem();
 }
 if(ImGui::BeginTabItem(i18n::Tr("对拍"))){
  ImGui::TextWrapped(i18n::Tr("当前标签为待测解；生成器通过第一个命令行参数接收种子，stdout 作为测试输入。首次失败停止并归档。"));
  auto path=[](const char* label,std::wstring& value){auto s=utils::WToU8(value);if(Field(label,s))value=utils::U8ToW(s);};path(i18n::Tr("参考解源码"),config.reference);path(i18n::Tr("生成器源码"),config.generator);
  ImGui::InputInt(i18n::Tr("轮数"),&config.rounds);ImGui::InputScalar(i18n::Tr("起始种子"),ImGuiDataType_U64,&config.seed);
  if(ImGui::Button(i18n::Tr("编译并开始对拍")))start(true);
  ImGui::EndTabItem();
 }
 ImGui::EndTabBar();
 }
 ImGui::EndDisabled();
 ImGui::Separator();ImGui::Text(i18n::Tr("得分: %.2f"),state.score);
 for(size_t i=0;i<state.results.size();++i){auto& result=state.results[i];ImGui::PushID(int(i));if(ImGui::TreeNode("##result","%s  %s  %llu ms  %.1f MiB",result.name.c_str(),result.verdict.c_str(),result.elapsed,result.peak/1048576.0)){ImGui::Text(i18n::Tr("退出码: %lu (0x%08lX)"),result.exitCode,result.exitCode);ImGui::TextWrapped("%s",result.termination.c_str());ImGui::TextWrapped("%s",result.difference.c_str());ImGui::TextUnformatted(result.error.c_str());ImGui::TextUnformatted(result.output.c_str());ImGui::TreePop();}ImGui::PopID();}
 }
 ImGui::End();
}
}
