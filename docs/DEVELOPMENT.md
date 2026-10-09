<p align="center"><img src="../assets/DeskFlow-150.png" width="80" alt="DeskFlow 标志"></p>

# 构建与验证

[English](DEVELOPMENT.en-US.md) · [项目首页](../README.md)

需要 Windows x64/ARM64 主机、Visual Studio 2022/2026 C++ 桌面工具及目标架构工具链、带 C++/WinRT 的 Windows SDK、CMake 3.24+（VS 2026 需要 4.2+）和 PowerShell。工程自带 SQLite 与 JSON 源码，运行版不依赖 Python、Node.js 或 .NET。

```powershell
git clone https://github.com/middletoo/DeskFlow.git
cd DeskFlow
pwsh -NoProfile -ExecutionPolicy Bypass -File scripts/build.ps1
```

默认构建 x64 Release 并执行 CTest。其他架构可加 `-Architecture x86` 或 `-Architecture ARM64`，默认输出分别为 `build-x86/Release`、`build-ARM64/Release`。x64 交叉编译 ARM64 时加 `-SkipTests`，再到 Arm Windows 运行测试。主程序旁保留 `DeskIndex.exe` 和 `DeskOCR.exe`。

## OCR 包身份

便携版可使用搜索、历史和截图。Windows OCR 需要注册 MSIX。维护者安装脚本固定特定证书和辅助程序，不要改成信任任意证书。

自行开发时，生成并检查证书后，在自己的开发设备确认信任该已知叶证书到 Trusted People，再注册包。

```powershell
pwsh -File scripts/package.ps1 -SkipBuild -CreateDevelopmentCertificate -Version 0.3.3.0
# 检查证书后，在管理员 PowerShell 中执行：
Import-Certificate -FilePath artifacts/msix/DeskFlow-Development.cer -CertStoreLocation Cert:\LocalMachine\TrustedPeople
Add-AppxPackage -Path artifacts/msix/DeskFlow-0.3.3.0-x64.msix
```

不要提交 PFX/私钥、个人设置或生成的信任材料。

## 维护者发布构建

`scripts/build-release.ps1` 构建三种架构，在本机证书库签名，校验固定安装器，并生成便携/安装压缩包和 SHA256 校验表。重新编译的辅助程序需审核并固定摘要后才能分发，不上传私钥。

Windows 工作流在 x64/x86 Windows 和 Windows 11 Arm 运行器执行编译及隔离测试；CI 产物未签名，Releases 中的签名压缩包在本机组装。

## 测试

```powershell
ctest --test-dir build -C Release --output-on-failure
```

测试覆盖历史/备份、设置/DPAPI、排序/续载、索引、窗口生命周期、OCR 身份、翻译取消、图像和 GIF/H.264/AAC。交互录制使用私人合成桌面并恢复；剪贴板写入使用独立窗口站；已有音频活动时跳过实际回环测试，合成声音不保存。

资源占用对照及开发者复测脚本见[性能说明](PERFORMANCE.md)。它使用全新合成数据，避免修改现有软件的配置及历史。

## 结构

| 模块 | 职责 |
| --- | --- |
| `app.cpp`, `panel_layout.hpp` | 托盘、快捷键、界面与有界任务 |
| `search.cpp`, `index_worker.cpp` | 只读查询及目录/NTFS 索引 |
| `clipboard.cpp`, `clipboard_preview.cpp` | 原格式存储与浮动预览 |
| `capture.cpp`, `image_tools.cpp` | 选区、贴图与长图 |
| `ocr_worker.cpp`, `translation.cpp` | 独立 OCR 与翻译请求 |
| `recording.cpp`, `system_audio.cpp` | 流式媒体与播放端点声音回环 |

WASAPI 回环读取默认播放端点，使用共享模式，不打开麦克风。实现依据微软[回环录制](https://learn.microsoft.com/en-us/windows/win32/coreaudio/loopback-recording)与 [AAC 编码器](https://learn.microsoft.com/en-us/windows/win32/medfound/aac-encoder)文档。
