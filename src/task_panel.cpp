#include "execution.h"
#include "utils.h"
#include <imgui.h>
#include <shellapi.h>
namespace execution {
void DrawTaskWindow(bool* open){
 static std::vector<TaskInfo> tasks;static ULONGLONG last=0;static bool activeOnly=false;static std::string message;
 ImGui::SetNextWindowSize(ImVec2(820,540),ImGuiCond_FirstUseEver);
 if(ImGui::Begin("任务管理",open)){
  ImGui::TextWrapped("同一 Windows 登录会话、同一用户共享名额：前台 2 / 后台检查 1 / 服务 2。排队可以取消，运行时限从实际启动后计算。");
  ImGui::Checkbox("仅显示活动任务",&activeOnly);ImGui::SameLine();if(ImGui::Button("刷新"))last=0;
  ImGui::TextWrapped("%s",message.c_str());
  if(GetTickCount64()-last>700){tasks=ListTasks();last=GetTickCount64();}
  ImGui::BeginChild("##task_list",ImVec2(0,0),ImGuiChildFlags_Borders);
  size_t shown=0;
  for(const auto& task:tasks){if(activeOnly&&!task.active)continue;++shown;ImGui::PushID(task.task.id.c_str());
   const char* state=task.state=="queued"?"排队":task.state=="starting"?"准备启动":task.state=="running"?"运行中":task.state=="completed"?"完成":task.state=="cancelled"?"已取消":task.state=="interrupted"?"实例已退出":"失败";
   if(ImGui::TreeNode("##task","%s · %s · %s",task.label.c_str(),task.lane.c_str(),state)){
    ImGui::TextWrapped("ID: %s",task.task.id.c_str());ImGui::Text("等待 %llu ms / 执行 %llu ms / 退出码 %lu",task.queuedMs,task.elapsedMs,task.exitCode);
    ImGui::TextWrapped("目录: %s",utils::WToU8(task.cwd).c_str());if(!task.error.empty())ImGui::TextWrapped("%s",task.error.c_str());
    if(task.active){if(ImGui::Button("停止此任务"))message=CancelTask(task.task.id)?"已发送取消请求":"任务可能已经结束";ImGui::SameLine();}
    if(ImGui::Button("打开任务记录"))ShellExecuteW(nullptr,L"open",task.task.directory.c_str(),nullptr,nullptr,SW_SHOWNORMAL);
    ImGui::TreePop();
   }ImGui::PopID();ImGui::Separator();
  }if(!shown)ImGui::TextDisabled("暂无符合条件的任务");ImGui::EndChild();
 }ImGui::End();
}
}
