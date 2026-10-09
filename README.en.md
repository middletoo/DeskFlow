<p align="center"><img src="assets/DeskFlow-150.png" width="108" alt="DeskFlow logo"></p>

# DeskFlow

[简体中文](README.md) · [User guide](docs/USER_GUIDE.en-US.md) · [Downloads](https://github.com/middletoo/DeskFlow/releases) · [Build from source](docs/DEVELOPMENT.en-US.md)

**Quick to open. Light to keep running.**

A native Windows tray utility for independent file search, persistent clipboard history, and screenshots with local OCR, image translation and recording.

Three global shortcuts bring up the tools. Lists load more as you scroll, clipboard entries open floating previews, and translations appear where the original text was. Native C++ and Windows APIs avoid an embedded browser runtime. Indexes and history stay on disk, caches have limits, and OCR and recording start on demand.

## Resource use: measured against Everything

### Observation of the existing applications on this computer

Existing configuration, indexes and history were retained. Both applications were observed for approximately 61.72 seconds, with CPU normalized to 32 logical processors. Each total includes two existing processes. No test searches were issued during sampling.

| Program | Mean CPU, whole machine | Mean private memory | Mean working set |
| --- | ---: | ---: | ---: |
| DeskFlow main app + index worker | 0.45% | 40.29 MiB | 47.44 MiB |
| Everything, two existing processes | 2.97% | 1677.25 MiB | 1665.01 MiB |

**In this observation, Everything used approximately 1.64 GiB of private memory, considerably more than DeskFlow's approximately 40 MiB.** The indexes cover different data: DeskFlow had about 575,000 entries and still reported background building/reconciliation. An earlier user-provided Everything screenshot showed about 17.6 million objects; its current count was not verified. CPU represents this minute's average, rather than long-term idle use. The gap cannot establish a corresponding advantage at equal index sizes.

Only aggregate values are published, excluding real filenames, paths, history and screenshots of the user's desktop. See [the observation and its conditions](docs/PERFORMANCE.en-US.md).

### Small same-folder idle benchmark

Both programs indexed **the same synthetic folder containing 12,000 files and 60 subfolders**, on one Windows x64 machine with 32 logical processors. After indexing completed, both were sampled together for three idle intervals of approximately 60 seconds. DeskFlow includes its main app and index worker; Everything includes the isolated instance.

**This is a hidden-UI file-index idle benchmark.** Neither main window was shown or painted. OCR, translation and recording were inactive. DeskFlow's test mode disabled clipboard reads and global hotkeys to avoid accessing local content or taking over the user's keys. These figures do not represent complete everyday use.

| Program (x64) | Mean CPU, whole machine | Mean private memory | Mean working set |
| --- | ---: | ---: | ---: |
| DeskFlow 0.3.3, two processes combined | 0.0003% | 10.41 MiB | 29.57 MiB |
| Everything 1.4.1.1030, one process | 0.0000%¹ | 6.25 MiB | 20.84 MiB |

Both programs' idle CPU use was close to zero in this benchmark. **Everything used less memory in this small file-index workload.** The DeskFlow figures exclude real clipboard history, a painted UI and capture tasks. Search latency, initial indexing performance, whole-volume NTFS/MFT mode and indexes containing millions of files were not measured; these results cannot establish performance in those scenarios.

¹ No measurable process CPU-time increase during the intervals, rather than literally no execution. Private memory and working set are distinct metrics; working set includes shared system pages. One MiB is 1,048,576 bytes. See [conditions, individual intervals and reproduction steps](docs/PERFORMANCE.en-US.md).

## Download and start

Prebuilt **Windows x64, x86 and ARM64** programs are available in [Releases](https://github.com/middletoo/DeskFlow/releases).

- **portable.zip**: extract everything and run `DeskFlow.exe`, keeping `DeskIndex.exe` and `DeskOCR.exe` beside it.
- **setup.zip**: extract everything and run `Install-DeskFlow.cmd`; this includes the MSIX identity needed for local OCR and image translation.
- Most Intel/AMD PCs use x64, 32-bit Windows uses x86, and Windows on Arm uses ARM64. No separate VC++ runtime is required.

Verify downloads with `SHA256SUMS.txt`. The current validation release uses a pinned self-signed development certificate; first installation may ask for UAC.

Supports Windows 10 2004+ and Windows 11. ARM64 validation uses Windows 11 on Arm. Local OCR needs MSIX package identity and Windows language resources. All screenshots below contain synthetic examples.

## One tray app, three shortcuts

| Function | Default shortcut |
| --- | --- |
| File search | **Alt+Q** |
| Clipboard history | **Alt+W** |
| Screenshot | **Alt+S** |

Hover over an entry to see its current shortcut; change it in Settings. The GitHub icon at the top right opens this repository.

## Independent file search

![File search](docs/images/files.png)

An independent disk index keeps existing entries searchable while indexing continues. Results support global sorting, incremental scrolling and a bounded cache. Name, directory, size and local modification time are visible by default. Folder size cells remain empty; older indexes backfill timestamps in the background.

Double-click a name to reveal a file; double-click its directory to copy the full file path. Windows context menus, multi-selection, drag-out, filters and export are available. Example searches: `report 2026`, `ext:pdf`, `type:folder`.

## Persistent clipboard history

![Clipboard history](docs/images/clipboard.png)

Preserve text, images, HTML/RTF and file lists in their original formats, with adjacent duplicate suppression, search and pinning. Entries are not automatically deleted because of their age. Content loads on demand from disk.

![Floating preview](docs/images/clipboard-preview.png)

Click an entry for a floating preview with selectable, scrollable text or bounded image thumbnails. Unsupported representations show details. Press Alt+W, then Enter to paste into the previous window.

## Capture and annotate

![Capture editor](docs/images/capture.png)

Window snapping, magnifier, adjustable regions, shapes, arrows, text, pen, mosaic and numbered annotations. A white grouped toolbar has a red cancel cross and a green copy check on its right. Undo and redo use keyboard shortcuts.

![Window hover and pixel information](docs/images/capture-hover.png)

Before the first left click, the outline follows the application beneath the pointer. Physical pixel coordinates and HEX color update even while moving within the same window. Ctrl+C copies the color; click to select a window or drag a custom region.

## Local text recognition

![Local OCR](docs/images/ocr.png)

Windows OCR runs in an isolated process on demand, with tiled recognition for long images. Select and copy the results. Press O in a selection, or import an image file or paste an image for recognition.

## Translate where the text was

![Image translation](docs/images/translation.png)

Press T in a capture selection to translate without opening the main app. Hold Space for the original, then annotate, copy, save or pin the translated image. Configure Google's experimental free channel, DeepL API, official Google API or LibreTranslate.

OCR runs locally; recognized text is sent to the selected translation service. Experimental free endpoints may change or rate-limit. Failed requests are not automatically forwarded to another service.

## Pin images and capture long content

![Pinned image](docs/images/pin.png)

Keep an image above other windows; move, zoom, change opacity or scroll it for quick reference.

![Scrolling capture](docs/images/scrolling.png)

Scroll manually inside the selected region to stitch reliable overlaps into a long image. Fixed headers or ambiguous overlap pause stitching.

## Record first, choose a file after stopping

![Recording controls](docs/images/recording.png)

GIF or MP4 starts immediately with an elapsed timer and pause, stop and cancel controls. Stop, then choose the location and filename; canceling the save dialog retains the clip for retry. **MP4 includes system playback audio by default**, including browser livestream sound. The microphone stays unopened; GIF has no audio.

The live recording region has a green outline, changing to amber when paused. Thin edges pass mouse input through and are excluded from the recording. Stop hides them before the save dialog. Frames stream to disk rather than keeping the entire recording in memory.

## Resource management for long-term use

- Native C++20, Win32, Direct2D/DirectWrite, SQLite, WIC, Media Foundation and WASAPI; no Electron runtime.
- Disk-backed indexes and history, with explicit budgets for list caches, images and recording tasks.
- Paced background indexing, filesystem notifications while idle, and on-demand OCR and media encoding.
- Updates retain package-family storage; API keys use Windows DPAPI protection.

Sustained 3% CPU while truly idle is high for a background utility. Initial indexing, repair, OCR and encoding need more resources. Check Tools → Index and runtime status, and see [performance notes](docs/PERFORMANCE.en-US.md). These choices and measurements do not guarantee crash-free multi-year use or identical responsiveness at every scale.

## Privacy and source

The public snapshot excludes personal files, real clipboard data, local logs and backups, API keys, signing private keys and local development history. Screenshots use synthetic examples; public commits use a GitHub noreply author address.

Source is published for inspection; no separate project license has been selected yet. Third-party terms are in [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).
