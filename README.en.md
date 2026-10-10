<p align="center"><img src="assets/DeskFlow-150.png" width="108" alt="DeskFlow logo"></p>

# DeskFlow

**A lightweight Windows tray utility, ready from a shortcut.**

Independent file search, persistent clipboard history, and capture with OCR, image translation and recording in one native desktop app.

[简体中文](README.md) · [Download](https://github.com/middletoo/DeskFlow/releases) · [User guide](docs/USER_GUIDE.en-US.md) · [Build guide](docs/DEVELOPMENT.en-US.md) · [Report an issue](https://github.com/middletoo/DeskFlow/issues)

## Features

- **File search:** independent indexing with NTFS MFT/USN, resumable background builds, sorting, filters, multi-selection and Windows context menus.
- **Clipboard history:** preserve text, images, rich text and file lists, with search, pinning and floating previews.
- **Capture tools:** window snapping, pixel color, annotations, local OCR, inline translation, pins, scrolling capture, GIF and MP4 with playback audio.
- **Low background overhead:** native C++ and Windows APIs, disk-backed data, bounded caches, and on-demand OCR and encoding.

## Download and install

Supports Windows 10 2004+ and Windows 11, with **x64, x86 and ARM64** builds. Most Intel/AMD PCs use x64; Windows on Arm uses ARM64.

1. Download your architecture from [Releases](https://github.com/middletoo/DeskFlow/releases).
2. For basic features, extract `portable.zip` and run `DeskFlow.exe`, keeping its worker programs beside it.
3. Recommended: download `setup.exe`, open the installation wizard and choose a data folder. Launch from Start for local OCR and image translation. The same wizard is also included in `setup.zip`.

No separate VC++ runtime is required. Local OCR needs MSIX identity and Windows recognition languages. First installation may display Windows UAC; the current installer uses a pinned self-signed development certificate. Verify downloads with `SHA256SUMS.txt`.

![Installation wizard: choose a data folder](docs/images/installer.png)

Keep databases, history attachments, settings and the index in a dedicated folder on another local drive. Changing location copies existing data and retains the source; unrelated nonempty destinations are rejected.

## Quick start

| Tool | Default shortcut | Common action |
| --- | --- | --- |
| File search | **Alt+Q** | Search a name or filter with `ext:pdf`, `type:folder`, etc. |
| Clipboard history | **Alt+W** | Click for a floating preview; Enter pastes to the previous window |
| Screenshot | **Alt+S** | Click a window or drag a region; the green check copies |

Hover over an entry to see its current shortcut; change keys in Tools → Settings. Closing the main window returns to the tray; use the tray menu to exit. Settings also offers startup after signing into Windows.

Fresh installations request administrator privileges for the **index worker** by default, while the main UI retains normal privileges. Existing explicit preferences are preserved and can be changed in Settings. Canceling UAC falls back to the current permissions. Existing results remain searchable during initial indexing; see Tools → Index and runtime status for coverage and progress.

## Screenshots

All screenshots use synthetic examples, excluding personal files and real clipboard content.

### File search

![File search](docs/images/files.png)

Name, directory, size and modification time are visible by default; folder sizes remain empty. Double-click a name to reveal a file or its directory to copy the full path. Global sorting, incremental scrolling, multi-selection, drag-out, Windows context menus, filters and export are available.

### Clipboard history and floating previews

![Clipboard history](docs/images/clipboard.png)

Text, images, HTML/RTF and file lists stay on disk, with adjacent duplicate suppression, search and pinning. Entries are not automatically deleted because of their age.

![Floating preview](docs/images/clipboard-preview.png)

Click for a floating preview with selectable, scrollable text or bounded image thumbnails. Unsupported representations show details.

### Capture, annotation and pixel color

![Capture editor](docs/images/capture.png)

A white grouped toolbar has a red cancel cross and green copy check. Shapes, arrows, text, pen, mosaic and numbered annotations are available, with keyboard undo and redo.

After confirming text, drag it to move or double-click to edit; changes support undo. Once a region is selected, shaded-area clicks preserve it. Use Esc or the cross, then Alt+S for a new capture.

![Window snapping and pixel information](docs/images/capture-hover.png)

Before the first left click, the outline follows the application beneath the pointer. The magnifier updates physical pixel coordinates and HEX color; Ctrl+C copies the color.

### Local OCR and image translation

![Local OCR](docs/images/ocr.png)

Press **O** in a selection to recognize text, then select and copy results. Windows OCR runs in an isolated process on demand, with tiled recognition for long images.

Bounded enlargement, dark/low-contrast preprocessing and wide-image tiling help preserve detail. Very small CJK glyphs, blurry images and unusual fonts can still be misread; check results before copying or translating.

![Image translation](docs/images/translation.png)

Press **T** to show translation over the corresponding image regions; hold Space for the original, then annotate, copy or save. Configure Google's experimental free channel, DeepL API, official Google API or LibreTranslate. OCR is local; recognized text is sent to the selected service.

### Pinned images and scrolling capture

![Pinned image](docs/images/pin.png)

Press **P** for an always-on-top image with movement, zoom, opacity and scrolling.

![Scrolling capture](docs/images/scrolling.png)

Press **L** and scroll manually to stitch a long image. Only reliable overlaps are stitched; fixed headers or ambiguous overlaps pause capture.

### GIF and video with playback audio

![Recording controls](docs/images/recording.png)

Press **G / M** to start GIF / MP4 immediately. The bar shows time, pause, stop and cancel. A green outline marks the live region and turns amber when paused; edges pass input through and stay out of the encoded picture. Stop, then choose a filename and location; canceling the save dialog allows retry.

**MP4 includes system playback audio by default**, including browser livestream sound. The microphone remains unopened; GIF has no audio. Frames stream to disk rather than keeping the full recording in memory.

## Resource use

Disk-backed indexes, bounded caches and paced background work reduce resident overhead. Usage varies with index size and the active task.

Observation of the pre-fix **0.3.3** applications on one computer, approximately one minute with different index coverage:

| Program | Mean CPU, whole machine | Mean private memory |
| --- | ---: | ---: |
| DeskFlow main app + index worker | 0.45% | 40.29 MiB |
| Everything, two existing processes | 2.97% | 1,677.25 MiB |

DeskFlow then had about 575,000 entries with indexing incomplete; an earlier Everything screenshot showed about 17.6 million objects. These are observations of that state. Default administrator indexing expands coverage in the new version; the table does not establish an equal-workload advantage or a resource ceiling for the new version. Conditions, the same-folder benchmark and reproduction scripts are in [performance notes](docs/PERFORMANCE.en-US.md).

## Documentation and development

- [User guide](docs/USER_GUIDE.en-US.md): search syntax, shortcuts, translation, recording limits and backups.
- [Build guide](docs/DEVELOPMENT.en-US.md): native builds, MSIX, tests and architecture.
- [Performance notes](docs/PERFORMANCE.en-US.md): measurements, index coverage and benchmark methods.

Development requires Windows, Visual Studio C++ desktop tools, Windows SDK and CMake. The application does not require Python, Node.js or .NET.

## Privacy and licensing

History and indexes stay on your computer; API keys use Windows DPAPI protection. Translation sends recognized text to the chosen provider; experimental free endpoints may change or rate-limit.

The public repository excludes personal files, real history, local logs, API keys, signing private keys and local development history. Public commits use a GitHub noreply address. Source is available for inspection; no separate project license has been selected. Third-party terms are in [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).
