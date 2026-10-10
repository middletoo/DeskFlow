<p align="center"><img src="../assets/DeskFlow-150.png" width="80" alt="DeskFlow logo"></p>

# Build and verify

[简体中文](DEVELOPMENT.md) · [Project overview](../README.en.md)

Use a Windows x64/ARM64 host, Visual Studio 2022/2026 C++ desktop tools including the target architecture, a Windows SDK with C++/WinRT, CMake 3.24+ (4.2+ for Visual Studio 2026) and PowerShell. SQLite and JSON sources are vendored. The application needs no Python, Node.js or .NET runtime.

```powershell
git clone https://github.com/middletoo/DeskFlow.git
cd DeskFlow
pwsh -NoProfile -ExecutionPolicy Bypass -File scripts/build.ps1
```

This defaults to x64, builds Release and runs CTest. Use `-Architecture x86` or `-Architecture ARM64` for other targets; their default outputs are `build-x86/Release` and `build-ARM64/Release`. Cross-compiling ARM64 on x64 requires `-SkipTests`; run the tests on native Arm Windows. Keep `DeskIndex.exe` and `DeskOCR.exe` beside `DeskFlow.exe`.

## OCR package identity

Portable search, history and capture work directly. Windows OCR needs a registered MSIX. Maintainer install scripts pin a specific certificate and helper; do not replace their pins with arbitrary certificates.

For your own development package, generate and inspect a development certificate, then explicitly trust only that known leaf in Trusted People on your development machine and register the package.

```powershell
pwsh -File scripts/package.ps1 -SkipBuild -CreateDevelopmentCertificate -Version 0.3.5.0
# After inspecting your certificate, use elevated PowerShell:
Import-Certificate -FilePath artifacts/msix/DeskFlow-Development.cer -CertStoreLocation Cert:\LocalMachine\TrustedPeople
Add-AppxPackage -Path artifacts/msix/DeskFlow-0.3.5.0-x64.msix
```

Do not commit PFX/private keys, personal settings or generated trust material.

## Maintainer release builds

`scripts/build-release.ps1` builds all three architectures, signs locally and produces standalone setup executables, portable/setup archives and SHA256 checksums. Setup embeds the signed MSIX, public certificate, pinned trust helper and backend script; `--validate-payload` verifies embedded content without installing or changing trust. A rebuilt trust helper must be reviewed and pinned before distribution. No private key is uploaded.

The Windows workflow builds and runs isolated tests on x64/x86 Windows and native Windows 11 Arm runners. Its artifacts are unsigned CI outputs; signed Releases archives are assembled locally.

## Tests

```powershell
ctest --test-dir build -C Release --output-on-failure
```

Tests cover history/backup, settings/DPAPI, sorting/continuation, indexing, UI lifetimes, OCR identity, provider cancellation, image tools and GIF/H.264/AAC media. Interactive recording tests use a private synthetic desktop and restore it. Clipboard writes use a separate window station. Live audio testing skips when another playback session is active; synthetic tones are not saved.

See [performance notes](PERFORMANCE.en-US.md) for the resource comparison and reproduction script. It uses fresh synthetic data rather than modifying existing application settings or history.

## Architecture

| Area | Responsibility |
| --- | --- |
| `app.cpp`, `panel_layout.hpp` | Tray, hotkeys, compact UI, bounded tasks |
| `search.cpp`, `index_worker.cpp` | Read-only queries, directory/NTFS indexing |
| `clipboard.cpp`, `clipboard_preview.cpp` | Durable formats, floating preview |
| `capture.cpp`, `image_tools.cpp` | Region editor, pins, scrolling capture |
| `ocr_worker.cpp`, `translation.cpp` | Isolated OCR and provider requests |
| `recording.cpp`, `system_audio.cpp` | Streaming media and playback-endpoint loopback |
| `setup.cpp`, `storage.cpp` | Native setup wizard, SQLite snapshot migration and data-folder selection |

WASAPI uses the default render endpoint in shared loopback mode and leaves the microphone unopened. See Microsoft's [loopback recording](https://learn.microsoft.com/en-us/windows/win32/coreaudio/loopback-recording) and [AAC encoder](https://learn.microsoft.com/en-us/windows/win32/medfound/aac-encoder) documentation.
