<p align="center"><img src="../assets/DeskFlow-150.png" width="80" alt="DeskFlow logo"></p>

# Build and verify / 构建与验证

Windows x64, Visual Studio 2022 C++ desktop tools, a current Windows SDK with C++/WinRT, CMake 3.24+ and PowerShell. SQLite and JSON sources are vendored. The application needs no Python, Node.js or .NET runtime.

需要 Windows x64、Visual Studio 2022 C++ 桌面工具、带 C++/WinRT 的 Windows SDK、CMake 3.24+ 和 PowerShell。工程自带 SQLite 与 JSON 源码，运行版不依赖 Python、Node.js 或 .NET。

```powershell
git clone https://github.com/middletoo/DeskFlow.git
cd DeskFlow
pwsh -NoProfile -ExecutionPolicy Bypass -File scripts/build.ps1
```

This configures x64, builds Release and runs CTest. Launch `build/Release/DeskFlow.exe`, with `DeskIndex.exe` and `DeskOCR.exe` beside it. / 脚本配置 x64、构建 Release 并执行测试；主程序旁保留索引和 OCR 工作程序。

## OCR package identity / OCR 包身份

Portable search/history/capture work directly. Windows OCR needs a registered MSIX. Maintainer install scripts pin a specific certificate/helper; do not replace their pins with arbitrary certificate files.

便携版可使用搜索、历史和截图。Windows OCR 需要注册 MSIX。维护者安装脚本固定特定证书和辅助程序，不要改成信任任意证书。

For your own development package, generate a development certificate, inspect it, then explicitly trust only that known leaf in Trusted People on your development machine and register the package. / 自行开发时，生成并检查证书后，在自己的开发设备确认信任该已知叶证书到 Trusted People，再注册包。

```powershell
pwsh -File scripts/package.ps1 -SkipBuild -CreateDevelopmentCertificate -Version 0.3.0.0
# Only after inspecting your certificate; elevated PowerShell / 检查证书后使用管理员 PowerShell：
Import-Certificate -FilePath artifacts/msix/DeskFlow-Development.cer -CertStoreLocation Cert:\LocalMachine\TrustedPeople
Add-AppxPackage -Path artifacts/msix/DeskFlow-0.3.0.0-x64.msix
```

Do not commit PFX/private keys, personal settings or generated trust material. / 不要提交 PFX/私钥、个人设置或生成的信任材料。

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
