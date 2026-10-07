<p align="center"><img src="assets/DeskFlow-150.png" width="108" alt="DeskFlow logo"></p>

# DeskFlow

**Quick to open. Light to keep running. / 快捷调起，轻量常驻**

A native Windows desktop utility for independent file search, persistent clipboard history, and screenshots with local OCR, image translation and recording.

Windows 原生桌面工具：独立文件搜索、长期剪贴板历史，以及截图、本地 OCR、原图翻译和录制。

**Low background overhead, fewer steps per task.** Use three global shortcuts, scroll to load more, click for a floating clipboard preview, and translate directly on the screenshot. / **常驻负担小，操作步骤少。** 三个全局快捷键随时调起，列表滚动续载，剪贴板点击浮动预览，截图直接在原位置显示译文。

[中文使用指南](docs/USER_GUIDE.zh-CN.md) · [English user guide](docs/USER_GUIDE.en-US.md) · [Build from source / 开发构建](docs/DEVELOPMENT.md)

## Small background footprint / 低资源占用

Native C++ and Windows APIs, without an embedded browser runtime. Indexes/history stay on disk; caches have limits; OCR and media work start on demand. Background indexing yields CPU, and idle monitoring waits for filesystem notifications.

原生 C++ 与 Windows API，无内嵌浏览器运行时。索引及历史存磁盘，缓存有界，OCR 与录制按需启动。索引主动让出 CPU，待机等待文件系统通知。

| 0.3.2 measured scenario / 实测场景 | CPU | Memory / 内存 |
| --- | --- | --- |
| Rendered UI hidden + empty-directory monitor, 60 s / 界面绘制后收回托盘、空目录监控 60 秒 | **0.0033%** combined / 合计 | **11.8 MiB** peak private / 私有内存峰值 |
| 12,000 synthetic files, indexing worker sampled for 8 s / 12,000 个合成文件、索引进程采样 8 秒 | **0.48%** | **5.6 MiB** private / 私有内存 |

Measured on a Windows test machine with 32 logical processors; CPU is normalized to the whole machine. These are specific fixtures, not limits for every workload. Idle working set was 40.9 MiB including shared system pages. See [measurement details / 性能说明](docs/PERFORMANCE.md).

测试机为 Windows、32 个逻辑处理器，CPU 按整机归一。以上是具体场景的测量值；待机工作集合计 40.9 MiB，含共享系统页。大量真实数据、图片预览、OCR、录制和初次建库需要更多资源，详见 [性能说明](docs/PERFORMANCE.md)。

## Download / 下载

Prebuilt **Windows x64, x86 and ARM64** programs are available in [Releases](https://github.com/middletoo/DeskFlow/releases). / 已编译的 **Windows x64、x86、ARM64** 程序见 [Releases](https://github.com/middletoo/DeskFlow/releases)。

- **portable.zip**: extract everything and run `DeskFlow.exe`; keep both worker EXEs beside it. / 完整解压后运行 `DeskFlow.exe`，保留旁边的两个工作程序。
- **setup.zip**: extract and run `Install-DeskFlow.cmd`; includes MSIX identity for local OCR and image translation. / 完整解压后运行 `Install-DeskFlow.cmd`，包含本地 OCR、原图翻译需要的 MSIX 包身份。
- x64 is for most Intel/AMD PCs; x86 for 32-bit Windows; ARM64 for Windows on Arm. No separate VC++ runtime is required. / 大多数 Intel/AMD 电脑选 x64；32 位 Windows 选 x86；Windows on Arm 选 ARM64，无需单独安装 VC++ 运行库。

Check download hashes with `SHA256SUMS.txt`. The validation installer uses a pinned self-signed development certificate and may ask for UAC on first installation. / 可用 `SHA256SUMS.txt` 校验下载；当前验证版使用固定的开发者自签名证书，首次安装可能请求 UAC。

> Validation release for Windows 10 2004+ / Windows 11; ARM64 validation uses Windows 11 on Arm. Local OCR requires MSIX package identity and Windows language resources. Screenshots below use synthetic data only.
>
> 当前为 Windows 10 2004+ / Windows 11 验证版；ARM64 在 Windows 11 on Arm 上验证。本地 OCR 需要 MSIX 包身份及系统语言资源。以下截图全部为合成示例。

## One tray app, three shortcuts / 一个托盘工具，三个入口

| Function / 功能 | Default shortcut / 默认快捷键 |
| --- | --- |
| File search / 文件搜索 | **Alt+Q** |
| Clipboard history / 剪贴板历史 | **Alt+W** |
| Screenshot / 截图 | **Alt+S** |

Hover over a function to see its current key. Keys can be changed in Settings. Click the GitHub icon at the top right to open this project. / 悬停功能入口显示当前快捷键，可在设置中修改；右上角 GitHub 图标一键打开当前项目。

## Find files without a separate search app / 独立文件搜索

![File search / 文件搜索](docs/images/files.png)

An independent disk index, read-only queries while indexing continues, global sorting, incremental scrolling and a bounded UI cache. Reveal files by double-clicking their name; copy a full path by double-clicking its directory. Windows context menus, multi-selection, drag-out, filters and exports are available.

独立磁盘索引，后台建库时仍可查询已有条目；支持完整集合排序、滚动续载及有界缓存。名称双击定位，目录双击复制完整文件路径，保留系统右键、多选、拖出、过滤和导出。

Size and local modification time are always visible; folder size cells stay empty. Click either header to sort the complete result set. Older indexes backfill timestamps in the background. / 默认显示大小和本地修改时间，文件夹大小栏留空；点击列标题对完整结果排序，旧索引的时间元数据在后台补齐。

## Keep clipboard history on disk / 长期保留剪贴板历史

![Clipboard history / 剪贴板历史](docs/images/clipboard.png)

Preserve text, images, HTML/RTF and file lists in their original formats, with duplicate suppression, search and pinning. No automatic deletion simply because an entry is old.

原格式保存文字、图片、HTML/RTF 和文件列表，支持连续去重、搜索与收藏，不因记录时间久就自动删除。

![Floating preview / 浮动预览](docs/images/clipboard-preview.png)

Click to preview content in a floating window; unsupported representations show details. / 点击浮动预览内容，不可呈现的格式显示详细信息。

## Capture and annotate / 截图与标注

![Capture editor / 截图编辑器](docs/images/capture.png)

Window snapping, magnifier, adjustable regions, shapes, arrows, text, pen, mosaic and numbered annotations. A white grouped toolbar, red cancel cross and green copy check keep the editor compact.

自动选窗、放大镜、选区调整、形状、箭头、文字、画笔、马赛克和序号标注。白色分组工具栏，红叉取消，绿色勾复制。

![Window hover and pixel information / 自动选窗与像素信息](docs/images/capture-hover.png)

Before clicking, the outline follows the application under the pointer. The magnifier updates physical pixel coordinates and HEX color even within the same window; Ctrl+C copies the color. Drag for a custom region or click to select the hovered window. / 按下左键前，选框跟随鼠标匹配应用窗口；同一窗口内移动也实时更新物理像素坐标和 HEX 色值，Ctrl+C 复制色值，单击选窗或拖动自定义选区。

## Local OCR / 本地文字识别

![Local OCR / 本地 OCR](docs/images/ocr.png)

On-demand isolated Windows OCR, including tiled long-image recognition. Text can be selected and copied. / 按需独立进程运行 Windows OCR，长图分块识别，结果可选择及复制。

## Translate where the text was / 原位置显示译文

![Image translation / 原图翻译](docs/images/translation.png)

Translate inside the capture editor, hold Space for the original, then annotate, copy or save the translated image. Google’s experimental free channel, DeepL API, official Google API and LibreTranslate are configurable.

在截图内完成翻译，按住 Space 对照原图，译图仍可标注、复制或保存。可配置 Google 免费实验通道、DeepL API、Google 官方 API 和 LibreTranslate。

## Pin and scroll / 贴图与长截图

![Pinned image / 置顶贴图](docs/images/pin.png)

Pin images above other windows; move, zoom, adjust opacity or scroll. / 贴图置顶，支持移动、缩放、透明度和滚动。

![Scrolling capture / 长截图](docs/images/scrolling.png)

Manually scroll a region and stitch reliable overlaps into a long image. / 在选区内手动滚动，按可信重叠拼接长图。

## Record first, choose a file after stopping / 先录制，停止后保存

![Recording controls / 录制控制条](docs/images/recording.png)

GIF or MP4 starts immediately with an elapsed timer and pause/stop/cancel controls. Stop, then choose a filename. Canceling the save dialog retains the clip for retry. MP4 includes default system playback audio; the microphone stays unopened. GIF has no audio.

GIF / MP4 点击后直接录制并计时，可暂停、停止和取消；停止后选择文件名，取消保存仍可重试。MP4 默认录入系统播放声音，麦克风不启用；GIF 无声音。

The live region has a green outline; pause changes it to amber. The thin edges pass mouse input through and are excluded from the recording. Stop hides them before the save dialog. / 实时录制范围显示绿色边框，暂停变琥珀色；窄边框不拦截鼠标、不写入录制文件，停止后在保存对话框出现前隐藏。

## Built for bounded background work / 面向长期使用的资源管理

- C++20, Win32, Direct2D/DirectWrite, SQLite, WIC, Media Foundation and WASAPI; no Electron runtime. / 原生 C++ 与 Windows API，不需要 Electron 运行时。
- History and indexes stay on disk; rows and image work have explicit budgets. / 数据在磁盘，列表缓存和图像任务有界。
- OCR and media encoding run only when needed. Frames stream to disk. / OCR 和编码按需启动，录制帧流式写盘。
- Updates retain package-family data; API keys use Windows DPAPI. / 更新沿用数据目录，API Key 使用 Windows DPAPI。

These are implementation choices, not a guarantee of crash-free multi-year use or Everything-equivalent performance at every scale. Initial indexing, HDR, complex translation layouts and mixed-DPI devices have practical limits described in the guides.

这些是资源管理设计，不是“多年永不崩溃”或“任何规模等同 Everything”的保证。首次索引、HDR、复杂翻译版面和混合 DPI 等实际边界见使用指南。

## Privacy and source / 隐私与源码

The public snapshot excludes personal files, real clipboard data, local logs/backups, API keys, signing private keys and local development history. All screenshots are generated from synthetic examples. Public commits use a GitHub noreply author address.

公开快照不包含个人文件、真实剪贴板、本机日志/备份、API Key、签名私钥和本地开发历史。截图均为合成示例，公开提交使用 GitHub noreply 地址。

Source is published for inspection; no separate project license has been selected yet. Third-party terms are in [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).

源码公开供查看，目前尚未指定独立项目许可。第三方许可见 [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md)。
