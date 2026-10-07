<p align="center"><img src="../assets/DeskFlow-150.png" width="80" alt="DeskFlow logo"></p>

# Build and verify / 构建与验证

Windows x64/ARM64 host, Visual Studio 2022/2026 C++ desktop tools (including the target architecture), a current Windows SDK with C++/WinRT, CMake 3.24+ (4.2+ for Visual Studio 2026) and PowerShell. SQLite and JSON sources are vendored. The application needs no Python, Node.js or .NET runtime.

需要 Windows x64/ARM64 主机、Visual Studio 2022/2026 C++ 桌面工具及目标架构工具链、带 C++/WinRT 的 Windows SDK、CMake 3.24+（VS 2026 需要 4.2+）和 PowerShell。工程自带 SQLite 与 JSON 源码，运行版不依赖 Python、Node.js 或 .NET。

```powershell
git clone https://github.com/middletoo/DeskFlow.git
cd DeskFlow
pwsh -NoProfile -ExecutionPolicy Bypass -File scripts/build.ps1
```

This defaults to x64, builds Release and runs CTest. Use `-Architecture x86` or `-Architecture ARM64` for other targets. Their default outputs are `build-x86/Release` and `build-ARM64/Release`. Cross-compiling ARM64 on x64 requires `-SkipTests`; run the tests on native Arm Windows. Launch `DeskFlow.exe` with `DeskIndex.exe` and `DeskOCR.exe` beside it.

默认构建 x64 Release 并执行 CTest。其他架构可加 `-Architecture x86` 或 `-Architecture ARM64`，默认输出分别为 `build-x86/Release`、`build-ARM64/Release`。x64 交叉编译 ARM64 时加 `-SkipTests`，再到 Arm Windows 运行测试。主程序旁保留索引和 OCR 工作程序。

## OCR package identity / OCR 包身份

Portable search/history/capture work directly. Windows OCR needs a registered MSIX. Maintainer install scripts pin a specific certificate/helper; do not replace their pins with arbitrary certificate files.

便携版可使用搜索、历史和截图。Windows OCR 需要注册 MSIX。维护者安装脚本固定特定证书和辅助程序，不要改成信任任意证书。

For your own development package, generate a development certificate, inspect it, then explicitly trust only that known leaf in Trusted People on your development machine and register the package. / 自行开发时，生成并检查证书后，在自己的开发设备确认信任该已知叶证书到 Trusted People，再注册包。

```powershell
pwsh -File scripts/package.ps1 -SkipBuild -CreateDevelopmentCertificate -Version 0.3.2.1
# Only after inspecting your certificate; elevated PowerShell / 检查证书后使用管理员 PowerShell：
Import-Certificate -FilePath artifacts/msix/DeskFlow-Development.cer -CertStoreLocation Cert:\LocalMachine\TrustedPeople
Add-AppxPackage -Path artifacts/msix/DeskFlow-0.3.2.1-x64.msix
```

Do not commit PFX/private keys, personal settings or generated trust material. / 不要提交 PFX/私钥、个人设置或生成的信任材料。

## Maintainer release builds / 维护者发布构建

`scripts/build-release.ps1` builds all three architectures, signs locally with the known maintainer certificate, validates the pinned installers, and creates portable/setup archives plus SHA256 checksums. A rebuilt helper must be reviewed and pinned in the installer before distribution. No private key is uploaded.

`scripts/build-release.ps1` 构建三种架构，在本机证书库签名，校验固定安装器，并生成便携/安装压缩包和 SHA256 校验表。重新编译的辅助程序需审核并固定摘要后才能分发，不上传私钥。

The Windows workflow builds and runs isolated tests on x64/x86 Windows and native Windows 11 Arm runners. Its artifacts are unsigned CI outputs; the signed Releases archives are assembled locally.

Windows 工作流在 x64/x86 Windows 和 Windows 11 Arm 运行器执行编译及隔离测试；CI 产物未签名，Releases 中的签名压缩包在本机组装。

## Tests / 测试

```powershell
ctest --test-dir build -C Release --output-on-failure
```

Tests cover history/backup, settings/DPAPI, sorting/continuation, indexing, UI lifetimes, OCR identity, provider cancellation, image tools and GIF/H.264/AAC media. Interactive recording tests use a private synthetic desktop and restore it. Clipboard writes use a separate window station. Live audio testing skips when another active playback session is present; synthetic tones are not saved.

测试覆盖历史/备份、设置/DPAPI、排序/续载、索引、窗口生命周期、OCR 身份、翻译取消、图像和 GIF/H.264/AAC。交互录制使用私人合成桌面并恢复；剪贴板写入使用独立窗口站；已有音频活动时跳过实际回环测试，合成声音不保存。

## Architecture / 结构

| Area / 模块 | Responsibility / 职责 |
| --- | --- |
| `app.cpp`, `panel_layout.hpp` | Tray, hotkeys, compact UI, bounded tasks / 托盘、快捷键、界面与有界任务 |
| `search.cpp`, `index_worker.cpp` | Read-only queries, directory/NTFS indexing / 只读查询及索引 |
| `clipboard.cpp`, `clipboard_preview.cpp` | Durable formats, floating preview / 原格式存储与浮动预览 |
| `capture.cpp`, `image_tools.cpp` | Region editor, pins, scrolling capture / 选区、贴图与长图 |
| `ocr_worker.cpp`, `translation.cpp` | Isolated OCR and provider requests / 独立 OCR 与翻译 |
| `recording.cpp`, `system_audio.cpp` | Streaming media and playback-endpoint loopback / 流式媒体与播放声音 |

WASAPI uses the default **render** endpoint in shared loopback mode, not a microphone. See [Microsoft loopback documentation](https://learn.microsoft.com/en-us/windows/win32/coreaudio/loopback-recording) and [AAC media types](https://learn.microsoft.com/en-us/windows/win32/medfound/aac-encoder).

回环读取默认播放端点，使用共享模式，不打开麦克风。实现依据微软回环与 AAC 文档。
