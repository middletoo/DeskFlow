<p align="center"><img src="../assets/DeskFlow-150.png" width="80" alt="DeskFlow logo"></p>

# Resource use and quick interaction / 资源占用与快捷交互

[Project overview / 项目首页](../README.md)

## Measurements / 实测

DeskFlow 0.3.2, 2026-10-07, Windows test machine with 32 logical processors. All benchmark data is synthetic; screenshots and local user data are excluded.

DeskFlow 0.3.2，2026-10-07，Windows 测试机，32 个逻辑处理器。工作负载全部为合成数据，没有公开用户文件、剪贴板或本机路径。

| Scenario / 场景 | Sample / 采样 | Normalized CPU / 整机 CPU | Private memory / 私有内存 |
| --- | --- | --- | --- |
| Main app + ordinary index helper, rendered then hidden UI, empty monitored root, no OCR/recording / 主程序及普通索引辅助程序，界面绘制后隐藏，监控空目录，没有 OCR/录制 | 60.109 s | 0.00325% combined / 合计 | 11.75 MiB combined peak / 合计峰值 |
| Ordinary index helper, 12,000 generated files / 普通索引辅助程序，12,000 个生成文件 | 8 s | 0.4761% | 5.62 MiB at sample end / 采样末值 |

Idle combined working-set peak was **40.93 MiB**; working set includes shared system DLL pages and differs from private memory. The active index test also required searchable progress, an indexed query under 500 ms and interruptible stop; observed stop took 47 ms.

待机工作集合计峰值 **40.93 MiB**，工作集包含共享系统 DLL 页，与私有内存是不同指标。活动索引测试同时检查结果持续可查、已索引查询少于 500 ms、停止可中断；该次停止耗时 47 ms。

CPU calculation: `100 × process CPU-time delta / wall seconds / logical processor count`. It describes the measured group and interval; brief bursts, different CPU counts and Task Manager's display may differ.

CPU 计算为 `100 × 进程 CPU 时间增量 / 墙钟秒数 / 逻辑处理器数`，仅对应被测进程及采样区间。短时峰值、处理器数和任务管理器显示口径可能不同。

## How to interpret 3% / 怎样看待 3%

Sustained 3% while doing nothing is high for a background desktop utility. Initial indexing, repair, OCR and video encoding perform real work; their short-term CPU use can be higher. Check Tools → Index and runtime status to identify the current phase.

对于后台常驻工具，真正待机时持续 3% 偏高。首次索引、校准、OCR、视频编码正在执行工作，短时占用可以更高；在“工具 → 索引与运行状态”确认当前阶段。

The indexing loop now uses actual thread CPU time to insert interruptible rests. Its normal background target is 0.5% of the machine, capped at a quarter of one core; large indivisible OS/SQLite operations can temporarily exceed it. Reducing CPU means initial indexing can take longer, while queries continue to use existing indexed rows.

索引循环现在依据实际线程 CPU 时间插入可中断等待，日常后台目标为整机 0.5%，最多按四分之一单核预算工作。单次较大的系统或 SQLite 操作可能暂时超过目标。减少 CPU 会延长初次建库时间，已有索引结果继续可查。

Local directory notification buffers are 256 KiB; network roots keep the required 64 KiB limit. The larger local queue reduces overflow repairs during file creation bursts. The recorder uses four slim indicator windows rather than a full-screen transparent surface, and redraws their color only when state changes.

本地目录通知缓冲区改为 256 KiB，网络目录保留 64 KiB 限制，减少大量文件变更导致的溢出校准。录制指示采用四条窄窗口边框，状态变化时才更新颜色，避免创建整屏透明图层。

## Fast interactions / 快捷操作

- Alt+Q / Alt+W / Alt+S open files, history and capture; hover shows the current key. / 三个全局快捷键调起文件、历史和截图，悬停显示当前按键。
- Double-click a filename to reveal it; double-click its directory to copy the full path. / 文件名双击定位，目录双击复制完整路径。
- Lists load more on scroll; clipboard clicks open floating previews. / 列表滚动续载，剪贴板点击浮动预览。
- Translate in the capture editor and hold Space for the original. / 截图内直接翻译，按住 Space 对照原图。
- Start recording immediately; green/amber outlines show the region and pause state. / 点击直接录制，绿色/琥珀色范围框表示录制/暂停。
- The GitHub icon opens this repository without adding another header row. / GitHub 图标打开当前仓库，不新增标题区域。

## Reproduce / 复测

Build from source, then run `python scripts/idle_probe.py` (Python is needed only for this developer probe), or `ctest --test-dir build -C Release -R index_background_cpu -V`. Private-desktop image tests should run serially.

构建后执行 `python scripts/idle_probe.py`（仅开发者测量脚本需要 Python），或 `ctest --test-dir build -C Release -R index_background_cpu -V`。使用私人桌面的图像测试应串行运行。
