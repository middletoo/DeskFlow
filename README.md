<p align="center"><img src="assets/DeskFlow-150.png" width="108" alt="DeskFlow 标志"></p>

# DeskFlow

**轻量常驻、快捷调起的 Windows 桌面工具。**

独立文件搜索、长期剪贴板历史，以及截图、OCR、原图翻译和录制，集中在一个原生托盘程序中。

[English](README.en.md) · [下载](https://github.com/middletoo/DeskFlow/releases) · [使用指南](docs/USER_GUIDE.zh-CN.md) · [构建指南](docs/DEVELOPMENT.md) · [问题反馈](https://github.com/middletoo/DeskFlow/issues)

## 功能特色

- **文件搜索**：独立索引，支持 NTFS MFT/USN、后台续建、排序、过滤、多选和系统右键菜单。
- **剪贴板历史**：文字、图片、富文本和文件列表原格式留存，支持搜索、收藏和浮动预览。
- **截图工具**：自动选窗、像素取色、标注、本地 OCR、原位置翻译、贴图、长截图、GIF 与有声 MP4。
- **低常驻负担**：原生 C++ 与 Windows API，数据存磁盘，缓存有界，OCR 和编码按需启动。

## 下载与安装

支持 Windows 10 2004+ / Windows 11，提供 **x64、x86、ARM64** 版本。大多数 Intel/AMD 电脑选择 x64，Windows on Arm 选择 ARM64。

1. 从 [Releases](https://github.com/middletoo/DeskFlow/releases) 下载对应架构。
2. 基础功能选择 `portable.zip`，完整解压后运行 `DeskFlow.exe`，保留旁边的工作程序。
3. 需要本地 OCR 和原图翻译时，选择 `setup.zip`，解压后运行 `Install-DeskFlow.cmd`，安装后从开始菜单启动。

无需单独安装 VC++ 运行库。本地 OCR 需要 MSIX 包身份和系统识别语言。首次安装可能显示 Windows UAC，当前安装包使用固定的开发者自签名证书；下载校验见 `SHA256SUMS.txt`。

## 快速上手

| 功能 | 默认快捷键 | 常用操作 |
| --- | --- | --- |
| 文件搜索 | **Alt+Q** | 输入名称，或使用 `ext:pdf`、`type:folder` 等过滤 |
| 剪贴板历史 | **Alt+W** | 点击浮动预览，Enter 粘贴到此前窗口 |
| 截图 | **Alt+S** | 单击选窗，或拖动框选；绿色勾复制 |

悬停功能入口显示当前快捷键，可在“工具 → 设置”修改。关闭主窗口收回托盘，托盘菜单可完全退出；设置中可开启登录 Windows 后自动启动。

新安装默认请求管理员权限运行**索引进程**，主界面保持普通权限。已有明确保存的设置会保留；可在设置中切换。取消 UAC 后按当前权限继续索引。首次建库期间已有结果可查，覆盖范围与进度见“工具 → 索引与运行状态”。

## 功能预览

所有截图均使用合成示例，不包含个人文件或真实剪贴板内容。

### 文件搜索

![文件搜索](docs/images/files.png)

默认显示名称、目录、大小和修改时间，文件夹大小留空。名称双击定位，目录双击复制完整路径；支持完整结果集排序、滚动续载、多选、拖出、系统右键、过滤和导出。

### 剪贴板历史与浮动预览

![剪贴板历史](docs/images/clipboard.png)

文字、图片、HTML/RTF 和文件列表保存在磁盘，支持连续去重、搜索与收藏，不因记录时间久就自动删除。

![浮动预览](docs/images/clipboard-preview.png)

点击记录弹出可滚动、选择文字的浮动预览；图片使用有界缩略图，不可呈现的格式显示详细信息。

### 截图、标注与取色

![截图编辑器](docs/images/capture.png)

白色分组工具栏，红叉取消、绿色勾复制。支持形状、箭头、文字、画笔、马赛克和序号标注，快捷键撤销与重做。

![自动选窗与像素信息](docs/images/capture-hover.png)

按下左键前，选框跟随鼠标匹配应用窗口；放大镜实时显示物理像素坐标和 HEX 色值，Ctrl+C 复制色值。

### 本地 OCR 与原图翻译

![本地 OCR](docs/images/ocr.png)

在选区按 **O** 识别文字，结果可选择与复制；Windows OCR 按需在独立进程运行，支持长图分块识别。

![原图翻译](docs/images/translation.png)

按 **T** 在截图对应位置显示译文，按住 Space 对照原图，继续标注、复制或保存。支持 Google 免费实验通道、DeepL API、Google 官方 API 和 LibreTranslate。OCR 在本地运行，识别出的文字发送给所选翻译服务。

### 贴图与长截图

![置顶贴图](docs/images/pin.png)

按 **P** 置顶贴图，支持移动、缩放、透明度和滚动。

![长截图](docs/images/scrolling.png)

按 **L** 开始手动滚动拼接长图；可靠的重叠才会拼接，固定标题或不明确的重叠会暂停。

### GIF 与有声视频录制

![录制控制条](docs/images/recording.png)

按 **G / M** 立即录制 GIF / MP4，控制条显示时间及暂停、停止和取消。绿色边框显示实时范围，暂停变琥珀色；边框不拦截鼠标，不进入录制画面。停止后选择文件名和保存位置，取消保存仍可重试。

**MP4 默认录入系统播放声音**，可录网页直播音频；麦克风不启用，GIF 无声音。帧流式写盘，避免整段录制驻留内存。

## 资源占用

DeskFlow 使用磁盘索引、有限缓存和分时后台索引，减少常驻负担。资源占用随索引规模和当前任务变化。

修复前 **0.3.3** 的本机运行观察（约一分钟、索引覆盖范围不同）：

| 程序 | 平均 CPU（整机） | 平均私有内存 |
| --- | ---: | ---: |
| DeskFlow，主程序与索引进程合计 | 0.45% | 40.29 MiB |
| Everything，两个现有进程合计 | 2.97% | 1,677.25 MiB |

当时 DeskFlow 约 57.5 万条目且索引未完成，Everything 此前截图约 1,760 万个对象。这组数据描述当时实际占用；新版默认管理员索引会扩大覆盖范围，不能将表格视为同等负载性能倍数或新版资源上限。完整条件、同目录基准和复测脚本见[性能说明](docs/PERFORMANCE.md)。

## 文档与开发

- [使用指南](docs/USER_GUIDE.zh-CN.md)：搜索语法、快捷键、翻译配置、录制限制和数据备份。
- [构建指南](docs/DEVELOPMENT.md)：原生编译、MSIX、测试和项目结构。
- [性能说明](docs/PERFORMANCE.md)：资源测量、索引覆盖与基准方法。

开发环境为 Windows、Visual Studio C++ 桌面工具、Windows SDK 和 CMake。运行版不依赖 Python、Node.js 或 .NET。

## 隐私与许可

历史与索引保存在本机，API Key 使用 Windows DPAPI 保护。翻译会向所选服务发送识别出的文字，免费实验通道可能变化或限流。

公开仓库不包含个人文件、真实历史、本机日志、API Key、签名私钥或本地开发历史，公开提交使用 GitHub noreply 地址。源码公开供查看，目前未指定独立项目许可；第三方许可见 [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md)。
