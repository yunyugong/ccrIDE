# 版本记录

完整发布说明和附件见 [GitHub Releases](https://github.com/yunyugong/ccrIDE/releases)。

## v0.3.8（候选构建 / candidate build）

全局搜索与头文件模型移到后台，新增搜索取消与过期结果丢弃；插件下载使用可取消的独立工作进程；默认构建改为 Release；增加可保存的工具栏文字标签选项。保留轻量补全、现有快捷键和拖动停靠。实现与边界见 [后台工作说明](docs/responsiveness-0.3.8.md)。正式附件以 GitHub Releases 为准。

Workspace search and header models run in the background. Marketplace downloads use a cancellable managed process. Release becomes the default build configuration, and toolbar text labels are configurable. Existing lightweight completion, shortcuts and draggable docking remain available.

## v0.3.7.2

新增跟随系统 / 简体中文 / English 语言选项、双语安装程序、GitHub ZIP 插件市场与社区地址 JSON 配置。语言切换立即生效，点击保存设置后保留。新增 [插件接口文档](docs/plugin-api.md)、[GitHub 托管与翻译贡献说明](docs/community.md) 和 [社区贡献指南](CONTRIBUTING.md)。

## v0.3.7.1

新增 25 项编辑与界面偏好、设置搜索、快捷预设、偏好导入导出及恢复默认。界面字号与编辑器字号独立，旧配置沿用兼容默认值。详见 [自定义设置](docs/customization.md)。

## v0.3.7

新增统一任务 ID、独立运行与临时目录，以及跨实例并发限制；从“运行 → 任务管理”查看和取消任务。使用方法及限制见 [任务调度](docs/task-coordinator.md)。AI 与 clangd 插件请使用 1.2.0 版。

## v0.3.6

修复恢复备份轮换、测试归档一致性，并收紧 DLL 依赖查找与重复 ID 检查。详情见 [0.3.6 修复记录](docs/reliability-0.3.6.md)。

## v0.3.5

新增多光标/列编辑、clangd 语义重命名预览、样例/Subtask/对拍和所有标签恢复。用法、验证范围及限制见 [工作流说明](docs/workflows.md)。

## v0.3.1 修复

- 本地插件在目标盘暂存，修复跨盘安装失败。
- 线上安装保留接口版本；下载和写入成功后再完成安装，失败清理临时文件。
- 插件加载失败可查看具体原因；缺少入口的 DLL 立即释放。
- 继续兼容 v0.3.0 的两个独立插件。
