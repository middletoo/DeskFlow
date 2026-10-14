<p align="center"><img src="../assets/DeskFlow-150.png" width="80" alt="DeskFlow logo"></p>

# Resource use and quick interaction

[简体中文](PERFORMANCE.md) · [Project overview](../README.en.md)

## 0.4.1: overnight recovery and continuous updates

Fixes Windows refusing replacement of mapped indexes while the UI is open, and filename candidates switching before a failed snapshot publication. Snapshot and lookup files now share a generation, published together by atomically switching `catalog.current`; readers can continue using the previous generation. The current and previous generations are retained; older released files are reclaimed. Legacy `catalog.index`/`catalog.lookup` and `files.db` remain compatible.

Restart coalesces updates per file and uses an index of directory children, avoiding replay of every intermediate change and repeated scans of all updates. One actual overnight log contained 392,938 blocks for 168,930 distinct files; the repaired first query slice took about 1.21 seconds including replay. This does not guarantee cold-cache performance or every log size. Its old filename lookup had a mismatched generation, so complete searches still needed scanning until a matching snapshot/lookup pair was published.

Compaction runs in a separate task while USN monitoring continues; changes are replayed from its starting cursor after publication. Update batches are flushed before saving cursors, reducing per-file synchronous writes. Status reads are independent of queries, existing object counts remain visible during replay/builds, and native startup no longer opens the legacy SQLite/WAL.

New regressions cover publication with active mapped readers, paired lookup switches, shorter log rotation and restart after repeated directory/child updates. A rebuild after a journal gap now starts at the current journal head; it no longer reuses the expired cursor and immediately rebuilds again. Background compaction can temporarily increase CPU, I/O and mapped working sets; older resource measurements apply only to their stated versions and conditions.

## 0.4.0: native NTFS catalog

Administrator indexing now obtains long filenames, hardlink names, sizes and modification times directly from the MFT. Raw metadata reads and the file-record API are supported; volumes without trustworthy raw reads use the file-record API. USN changes update the affected files rather than scheduling whole-volume directory scans for hardlink changes.

Names and metadata use a compact mapped snapshot with shared directory paths and prebuilt name, path, size, type, modification-time and ID orders. A compressed filename candidate index avoids scanning millions of names for sparse matches. Query continuations remain stable during writes. Checksummed update blocks support recovery and bounded log compaction; a lost journal checkpoint explicitly triggers a new trustworthy snapshot.

Local validation on three NTFS volumes with about 6.88 million objects:

| Measurement | Result |
| --- | ---: |
| Launch to completed main snapshot file | 151.49 s |
| Separate filename lookup construction | 58.74 s |
| Main snapshot size | 1,202,847,744 bytes |
| Filename lookup size | 225,437,770 bytes |
| Subsequent sorted queries for “AI 金融” | About 3–6 ms; 8 results |
| Actual create, modify, delete, rename, directory move and hardlink updates | About 143–341 ms in this run |

The two build stages were measured in separate invocations. Their roughly 210-second sum is an estimate, not one end-to-end measurement. The previous implementation was still traversing directories after 25 hours. All three volumes completed in this validation, with no unresolved paths reported. Coverage refers to the selected local NTFS volumes; offline and network locations are excluded. An Everything count that also contains overlapping folder indexes is not a comparable unique-object count.

The first query in a new process, including loading updates, took about 332 ms. The subsequent millisecond measurements do not guarantee cold startup or every complex filter. Broad matches, path searches, regex and many updates can cost more.

Mapped pages consume real system memory and working-set space. **Low private memory does not mean the same low total RAM cost.** The implementation no longer treats 80 MiB as a fixed ceiling for multimillion-object indexes. Usage depends on object count, names, orders and cached pages. Catalog construction uses one compute thread; file-record reads and media encoding have separate costs.

Upgrades retain history, settings and the selected data folder. The old `files.db` remains available. Native indexing uses `catalog.index`, `catalog.lookup`, `catalog.delta` and `catalog.status`; directory indexing remains available for ordinary roots or insufficient privileges. Existing results remain available while the first native snapshot is built.


## 0.3.8: restart and administrator indexing

When Windows redirects the install directory, elevation uses the physical executable path to avoid access-denied failures through the logical path. The app checks the launched process token before reporting administrator indexing. UAC cancellation, launch failures and unverified privileges have explicit status messages. Existing databases and build checkpoints are retained; upgrading does not intentionally rebuild the index. Windows UAC still requires user confirmation.

Within one USN read, repeated content, timestamp and close notifications for the same ordinary file are combined into one update. Create, delete, rename, hardlink and directory events remain ordering barriers. This avoids duplicate metadata reads and database writes without dropping real namespace changes.

## 0.3.7: initial construction and live updates

Fixed mismatched outer-loop and SQLite CPU budgets: the inner callback previously kept the daily-maintenance throttle during initial construction. Native writes now supply derived fields on insertion, avoiding redundant row updates. Construction uses a bounded 32 MiB page cache and normal I/O priority, restoring a 4 MiB cache and background priority afterward. Initial CPU has a one-core ceiling; daily maintenance keeps its 0.5%-of-machine, quarter-core budget.

Deletion, rename and inaccessible-directory protection now seek the exact path and subtree ranges. The previous OR predicate used only the root portion of the index and scanned every object in that volume for each event, blocking journal replay on large indexes. A regression check bounds the database work for missing paths. On the same approximately 793,000-row root, twelve alternating read-only missing-path samples measured a median of 187.28 ms for the old predicate and 0.029 ms for two bounded seeks. This lookup-only check excludes deleting existing rows and maintaining FTS.

Raw MFT enumeration takes priority before path construction. Larger transactional path batches rotate between volumes; whole-root metadata/hardlink scans wait for native names to finish. Pending updates select eligible roots by key instead of scanning queues belonging to unenumerated volumes, and directory queues use an ordered index. These stages retain separate checkpoints.

USN changes are processed during enumeration and path construction, with separate durable journal and build cursors. Creates, deletes, renames and modification-time changes no longer wait for the initial build to finish. The visible UI checks lightweight revision metadata every 250 ms and refreshes file queries when data changes while retaining scroll and selection state. Busy disks, OS calls and event backlogs can still increase latency; this is not a hard real-time guarantee.

Local validation on 32 logical processors:

| Workload | Result |
| --- | ---: |
| Old synthetic path construction, 20,400 objects, original pacing | 31.05 seconds |
| New same-size synthetic workload, production FTS and sort indexes | 5.87 seconds |
| Old running index, 20-second observation | About 102 rows/second |
| New private index copy, 101,376 resumed objects | 34.26 seconds, about 2,959 rows/second |

The synthetic workload measures path construction, excluding volume enumeration, directory/hardlink reconciliation and complete GUI latency. Live and copied-index conditions differ in caches, concurrent load and scope, so the last two rows are not a controlled A/B comparison. These measurements do not guarantee full-build duration or Everything-equivalent speed. Real filenames, paths and benchmark databases are never published.

## Version and index coverage

Both measurement sets below come from pre-fix **0.3.3**. Fresh **0.3.4** installations request administrator indexing by default, preserve unfinished directory queues and MFT build checkpoints, and use USN monitoring on NTFS to reduce duplicate whole-volume reconciliations. Metadata enumeration privileges and reparse-directory classification expand coverage.

The older live index had three incomplete directory-mode roots, about 56,000 queued directories and 69 reported access-denied directories. Its smaller entry count and memory footprint partly reflected incomplete coverage, so they cannot establish an equal-scale performance advantage. A complete equal-scale measurement of the new version has not been established; the older results are not its resource ceiling.

## Existing application observation: large indexes with different coverage

Existing instances, window state, configuration, indexes and history were retained. Memory was sampled every second for 61.72 seconds; CPU is normalized to 32 logical processors. Each application total includes two processes. DeskFlow did not use the test mode that disables clipboard reads and hotkeys. No search, capture, OCR, translation or recording tests were issued during sampling.

| Program (x64) | Mean CPU, whole machine | Mean private memory | Mean working set |
| --- | ---: | ---: | ---: |
| DeskFlow 0.3.3.0, main app + index worker | 0.45% | 40.29 MiB | 47.44 MiB |
| Everything 1.4.1.1030, two existing processes | 2.97% | 1677.25 MiB | 1665.01 MiB |

Everything used about 1.64 GiB of private memory, approximately 41.6 times DeskFlow's total, and more CPU during this observation. **This describes the computer's current state, rather than an equal-workload benchmark.**

- Only DeskFlow's aggregate entry count and building flag were read: 574,720 entries, with background building/reconciliation still active.
- An earlier user-provided Everything screenshot showed approximately 17.6 million objects. This is an approximate prior screenshot count; the live count request did not return, so the current count was not verified.
- Coverage and configuration differ. Everything's specific background phase was not confirmed, so this interval is not described as idle CPU use.
- Approximately one minute cannot establish long-term stable usage, equivalent search speed or an equal-scale resource advantage.
- Existing applications remained running; only the unresponsive count-probe client was stopped. Usernames, filenames, paths, configuration, clipboard contents and desktop screenshots are not published.

See the sanitized [live observation data](benchmarks/live-resource-observation.json).

### Why the small-folder benchmark differs

Index size, indexed fields, sorting settings and current activity affect resource use. Everything's documentation states that its running database stays in memory and that extra metadata and fast-sort indexes require additional memory. Its 12,000-file footprint cannot represent an index of millions of objects. [Everything index documentation](https://www.voidtools.com/support/everything/indexes/)

DeskFlow uses a disk-backed SQLite index and bounded caches to reduce resident overhead. Equal-coverage indexing and measurement are still needed to establish equivalent responsiveness and resource benefits at millions of entries. The current smaller footprint does not replace that validation.

## Same-folder comparison with Everything

This benchmark compares DeskFlow **0.3.3.0 x64** with Everything **1.4.1.1030 x64**, on one Windows x64 machine with 32 logical processors. All files are generated by the script; personal files, clipboard entries and existing application configuration databases are not read.

| Program | Mean CPU, whole machine | Mean private memory | Sampled private-memory peak | Mean working set |
| --- | ---: | ---: | ---: | ---: |
| DeskFlow main app + index worker | 0.0003% | 10.41 MiB | 10.56 MiB | 29.57 MiB |
| Everything isolated instance | 0.0000% | 6.25 MiB | 6.40 MiB | 20.84 MiB |

Both programs' idle CPU use was close to zero here; Everything used less memory. Its zero CPU value means there was no measurable process CPU-time increase during the intervals, rather than literally no execution. DeskFlow's additional features were inactive, so this table does not compare the resource cost of complete functionality.

### Conditions

- One synthetic folder: 12,000 small text files in 60 subfolders, with 200 files per subfolder. Everything also counts the root itself, returning 61 folders; DeskFlow returns 60. Both return the same file count.
- Ordinary folder indexing and directory change monitoring, with file size and modification time enabled. No whole-volume NTFS/MFT indexing, administrator privileges or service processes.
- Fresh isolated configuration and databases. Sampling begins five seconds after both indexes finish and their counts are verified.
- Neither main window was shown or painted. No searches, OCR, translation, capture or recording ran; DeskFlow's history database was empty.
- DeskFlow runs with `--tray --idle-test`, which disables clipboard listening/reads and global hotkey registration to avoid accessing local content or interfering with existing applications.
- Everything uses an existing executable copied into the test directory, with a named instance. Automatic whole-volume inclusion, update checks and network services are disabled. The original settings remain unchanged.
- Three owned processes are sampled together, with DeskFlow's two processes summed at each point. Memory is sampled every second for three intervals of approximately 60 seconds each.

[Benchmark data](benchmarks/resource-comparison.json) contains raw values, binary SHA256 fingerprints and environment information without personal identifiers.

### Individual intervals

The intervals are consecutive within one launch, rather than three cold starts. Overall means average the three interval means. Sampled peaks are the largest one-second observations, not the operating system's peaks since startup.

| Interval | Duration | Program | CPU, whole machine | Mean private memory | Mean working set |
| --- | ---: | --- | ---: | ---: | ---: |
| 1 | 60.031 s | DeskFlow combined | 0.0008% | 10.49 MiB | 29.61 MiB |
| 1 | 60.031 s | Everything | 0.0000% | 6.33 MiB | 20.88 MiB |
| 2 | 60.031 s | DeskFlow combined | 0.0000% | 10.40 MiB | 29.57 MiB |
| 2 | 60.031 s | Everything | 0.0000% | 6.24 MiB | 20.84 MiB |
| 3 | 60.016 s | DeskFlow combined | 0.0000% | 10.33 MiB | 29.54 MiB |
| 3 | 60.016 s | Everything | 0.0000% | 6.18 MiB | 20.82 MiB |

CPU: `100 × summed process CPU-time delta / wall seconds / logical processor count`, using Windows `GetProcessTimes`. Memory uses `GetProcessMemoryInfo`: private memory is process-private committed memory, while working set consists of resident physical pages, including shared system pages. They are not interchangeable. Summed working sets may count shared pages more than once. One MiB equals 1,048,576 bytes.

### Scope

This is a **small, hidden-UI, already-indexed idle benchmark**. It does not measure initial indexing time, search latency, a displayed UI, growing history, whole-volume mode, millions of entries or OCR/recording peaks. Full clipboard monitoring, previews and displayed windows can add overhead. These figures cannot establish long-term everyday resource use.

The existing instances observed above cover different data; they describe usage at that time rather than equal-workload efficiency. Processor count, application version, configuration and OS caching affect the results. Repeated measurements should verify counts and indexing mode.

## How to interpret 3% CPU

Sustained 3% while truly idle is high for a background desktop utility. Initial indexing, repair, OCR and encoding perform real work and can use more CPU temporarily. Check Tools → Index and runtime status before assessing the current phase.

DeskFlow currently manages background work with:

- Interruptible rests based on actual indexing thread CPU time. The normal background target is 0.5% of the machine, capped at a quarter of one core. Large indivisible OS or SQLite operations can temporarily exceed it.
- Writer-side SQLite progress callbacks that yield during long statements; foreground read-only queries have separate budgets. Startup cleanup uses the name index to avoid extra full-text queries.
- 256 KiB local directory notification buffers, with the 64 KiB limit retained for network roots, reducing overflow repairs during file change bursts.
- Disk-backed indexes/history and bounded row/image work; on-demand OCR and encoding, with recording frames streamed to disk.
- Four slim recording indicator borders, updated when state changes.

Reducing background CPU can extend initial indexing time; existing indexed rows remain searchable. Budgets and observed measurements are not guarantees for every machine or workload.

## Quick interactions

- Alt+Q / Alt+W / Alt+S open files, clipboard history and capture; hovering shows the current shortcut.
- Double-click a name to reveal a file, or its directory to copy the full path.
- Lists load on scroll; clipboard clicks open floating previews.
- Translate within capture and hold Space for the original.
- Recording starts immediately, with green/amber outlines for recording/paused.
- The GitHub icon opens this repository.

## Reproduce

Build Release using the [development guide](DEVELOPMENT.en-US.md). Prepare an existing Everything executable and the official [ES command-line tool](https://www.voidtools.com/en-us/support/everything/command_line_interface/). Python is only needed for the developer probe; DeskFlow itself does not require it.

From the repository root, replace the example paths with your own installation locations:

```powershell
python scripts/compare_resources.py --everything "C:/Tools/Everything/Everything.exe" --es "C:/Tools/Everything/es.exe" --desk-directory "build/Release" --files 12000 --seconds 60 --runs 3
```

The script creates synthetic data, isolated indexes and a report under `artifacts/validation/comparison`. It waits for complete indexes before measuring and exits only its own processes. Original configuration and history are not copied or modified; generated test directories are retained. Everything's options follow its official [command-line documentation](https://www.voidtools.com/support/everything/command_line_options/) and [INI settings](https://www.voidtools.com/en-uk/support/everything/ini/).

Run `ctest --test-dir build -C Release -R index_background_cpu -V` for the background indexing budget test, or `python scripts/idle_probe.py` for a standalone test that paints the UI before hiding it. Those workloads differ and should not be mixed into this comparison. Private-desktop image tests should run serially.
