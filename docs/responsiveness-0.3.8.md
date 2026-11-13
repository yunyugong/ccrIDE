# v0.3.8：后台工作与取消 / Background work and cancellation

本文件描述 0.3.8 的实现；正式下载以 GitHub Releases 上实际发布的附件为准。

## 全局搜索 / Workspace search

目录遍历与文件读取在专用线程中进行。界面显示已扫描文件数、结果数与取消状态；新搜索替换旧搜索，清空后旧结果不会重新出现。使用文件快照，不访问打开标签的可变文本。

单文件最多 2 MiB、最多 4000 条结果，每条摘录最多 1024 UTF-8 字节。跳过隐藏目录、常见构建目录、IDE 数据/备份目录和重解析点（包括目录联接）。大小写折叠限于 ASCII；不是完整的 Unicode 语言搜索。取消为协作式，操作系统阻塞的文件系统调用仍可能延迟退出。

Traversal and file reads run on a dedicated worker. A newer query supersedes its predecessor; cleared results cannot be repopulated by an old job. The UI shows scanned files, matches and cancellation state. Limits: 2 MiB per file, 4000 matches, 1024 UTF-8 bytes per excerpt. Hidden/build/data directories and reparse points are excluded. Case folding is ASCII only. Cancellation is cooperative; a blocked OS filesystem operation may delay shutdown.

## 头文件模型 / Header models

编译器 include 路径查询与头文件翻译单元解析使用单独的后台线程。任务持有文档、路径、编译器的快照；只接收与当前签名一致的结果。可从状态栏取消索引；关闭标签或提交新修订也会取消该文档的旧任务。队列与未领取结果各限制为 8 项，编译器目录缓存最多 4 项。界面继续使用轻量补全，头文件模型就绪后刷新候选。编译器查询受执行管理器的超时、队列与取消约束。

Compiler include discovery and translation-unit parsing run on an independent worker. Each job owns snapshots of the document, path and compiler; only a matching revision can be applied. The status bar can cancel indexing; closing a tab also cancels its pending work. Requests and unclaimed results are bounded to eight each, with four cached compiler directory lists. Existing lightweight completion remains available while headers load. A pending completion session refreshes when its header model becomes available. This remains a heuristic C++ model; use clangd for complex semantic analysis.

## 插件下载 / Plugin downloads

市场 HTTP 请求在主程序启动的独立下载进程中执行，进程不初始化界面或加载插件。任务管理器负责 Job、取消、130 秒执行上限、384 MiB 提交内存上限与 64 KiB 诊断输出上限；等待服务槽位仍有独立队列超时。退出 IDE 会取消下载，即使 HTTP 请求正在等待响应头或响应体，也不必等网络超时。下载数据仍检查大小、SHA-256 和插件清单。

ZIP 解压支持取消；安装提交前再次检查取消。目录原子重命名成功后表示安装已经提交，后到的取消不回滚已完成安装。下载临时文件和失败安装的暂存目录清理；任务记录继续保留供诊断。此改动仅隔离市场下载，不代表第三方 DLL 已经沙箱化。

Marketplace HTTP requests run in a managed child process before UI/plugin initialization. Job cancellation and a 130-second execution deadline cover blocked response headers and body reads. Limits are 384 MiB committed memory and 64 KiB diagnostic output; service-slot queue time has its own deadline. Downloads retain size, SHA-256 and manifest checks. Cancellation is checked during ZIP extraction and immediately before committing the install. A completed atomic directory rename is the commit point; a later cancellation does not undo success. Transient responses and failed staging trees are removed, while task records remain available. This isolates downloading, not arbitrary in-process DLL plugins.

## 构建与界面 / Builds and UI

默认 Release；显式 Debug/RelWithDebInfo/MinSizeRel 仍可选择。工具栏文字标签可在设置中关闭，窄窗口自动收起；F10/F11/F12 与拖动停靠保持原行为。英文与中文界面均提供新增设置及状态文案。

Release is selected by default; explicit Debug/RelWithDebInfo/MinSizeRel configurations remain available. Toolbar text labels are configurable and collapse when space is limited. F10/F11/F12 and draggable docking retain their existing behavior. New settings and status messages have Chinese and English translations.

## 验证 / Validation

回归覆盖搜索替换/清空/取消、边界与摘录 UTF-8、头文件来源定位、过期修订丢弃，以及服务器分别阻塞响应头与响应体时的取消、退出清理和再次下载。性能数据与测量限制见 [performance.md](performance.md)。

Regression coverage includes query replacement/clear/cancel, bounded UTF-8 excerpts, header source mapping, stale revisions, and network cancellation/shutdown/retry against stalled headers and bodies. See [performance.md](performance.md) for measurements and their limits.
