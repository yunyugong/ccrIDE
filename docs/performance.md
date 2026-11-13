# 可比较核心性能记录

测量日期：2026-11-07。Windows 10.0.22631（Python 平台字符串），AMD Ryzen 5 7500F，物理内存约 15.6 GiB，GCC 11.5.0。两个构建使用相同 CMake 空 BuildType / 空 CXX_FLAGS（未优化构建）、相同依赖与插件关闭状态；不能将这些数字当作优化后的 Release 指标。

0.3.4 基线来自提交 7375a0647f2f083139cbaf64f7f12140f18eec48 的源码快照，加入同一测量入口；0.3.5 使用当前源码。原始数据保留可执行文件 SHA256，见 [performance-data.json](performance-data.json)。

工作负载：构造固定重复 int 声明文本，依次执行 SetText、RebuildLines、CodeModel Ensure。每档七次，报告中位数和最大观测值；样本太少，不将最大值解释为稳定 p95。私有内存为该档操作完成并销毁编辑器后的采样，峰值工作集为进程累计峰值，不是空闲 IDE 占用。

| 文本字节 | 0.3.4 中位数 ms | 0.3.5 中位数 ms | 0.3.4 最大值 ms | 0.3.5 最大值 ms |
|---|---:|---:|---:|---:|
| 100008 | 7.36 | 6.45 | 8.38 | 7.37 |
| 1000008 | 63.15 | 60.25 | 66.65 | 63.48 |
| 4000032 | 245.84 | 246.04 | 253.46 | 251.15 |

在此单机、单一合成工作负载下没有观察到明显回退；小幅差异可能来自缓存、调度和噪声，不作为性能优化结论。

## 复验

分别在基线与当前源码中构建 core_benchmark（相同 tests/core_benchmark.cpp 与依赖/编译选项），随后运行：

    python tests/compare_benchmarks.py --baseline <基线 EXE> --current <当前 EXE> --output <新 JSON>

测量时停止其他构建和测试。脚本保存硬件、系统、数据集、样本数和程序摘要。后续可用同一入口增加真实源文件集；不要用普通回归测试耗时替代此基准。

尚缺少：真实冷启动、空闲 CPU、完整 UI 操作延迟、磁盘打开、clangd/工程索引、长时间内存与不同硬件数据。故总体性能验收仍为部分完成。

## v0.3.8 Release 对照（2026-11-13）

同一台 Ryzen 5 7500F / Windows 11 / GCC 11.5.0 上重跑相同核心操作入口。基线为上文 SHA-256 一致的 0.3.5 历史未优化测量程序；新版为 0.3.8 Release（`-O3 -DNDEBUG`）。原始结果、二进制摘要和限制见 [performance-release-0.3.8.json](performance-release-0.3.8.json)。

| 文本字节 | 历史未优化中位数 ms | 0.3.8 Release 中位数 ms | 新版最大观测值 ms |
|---|---:|---:|---:|
| 100008 | 6.91 | 2.87 | 3.47 |
| 1000008 | 64.15 | 25.50 | 27.52 |
| 4000032 | 255.87 | 104.45 | 108.55 |

版本和优化选项同时变化，不能将差异归因于某一处源码改动；也不代表冷启动、真实项目索引或界面延迟的倍数提升。每档仅 7 次，最大值不是经统计确认的 p95。旧的未优化数据继续保留。

复验：`python tests/compare_release_benchmarks.py --baseline <归档0.3.5测量程序> --current <Release core_benchmark.exe> --output <新JSON>`。停止其他构建和测试后测量；脚本首先核对历史基线 SHA-256。

English: the same synthetic core-operation harness was measured against an archived unoptimized 0.3.5 binary and the 0.3.8 Release binary. Version and compiler optimization both differ. These results do not establish a source-only speedup, cold-start improvement, project-indexing performance or UI latency. Seven samples per size are insufficient for a statistically established p95; raw data reports the maximum observation explicitly.
