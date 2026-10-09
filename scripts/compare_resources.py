"""Compare isolated DeskFlow and Everything instances on one synthetic folder.

Requires Windows, Python, existing DeskFlow binaries, Everything.exe and the
official ES command-line client. No user settings, history or filenames are read.
"""
import argparse
import ctypes
from ctypes import wintypes
import hashlib
import json
import os
from pathlib import Path
import shutil
import sqlite3
import statistics
import subprocess
import time
import uuid

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--everything", type=Path, required=True)
parser.add_argument("--es", type=Path, required=True)
parser.add_argument("--desk-directory", type=Path, default=Path(__file__).resolve().parents[1] / "build/Release")
parser.add_argument("--files", type=int, default=12000)
parser.add_argument("--seconds", type=int, default=60)
parser.add_argument("--runs", type=int, default=3)
parser.add_argument("--output", type=Path, default=Path(__file__).resolve().parents[1] / "artifacts/validation/comparison")
args = parser.parse_args()
if os.name != "nt" or min(args.files, args.seconds, args.runs) <= 0:
    raise SystemExit("Windows and positive fixture/sample sizes are required")
for binary in (args.everything, args.es, args.desk_directory / "DeskFlow.exe",
               args.desk_directory / "DeskIndex.exe"):
    if not binary.is_file():
        raise SystemExit("Required benchmark executable is missing: " + binary.name)
args.output = args.output.resolve()
args.output.mkdir(parents=True, exist_ok=True)
fixture = args.output / ("fixture-" + uuid.uuid4().hex)
root = fixture / "files"
desk_data = fixture / "desk-data"
everything_dir = fixture / "everything"
for folder in (root, desk_data, everything_dir):
    folder.mkdir(parents=True)
for index in range(args.files):
    folder = root / ("group-%03d" % (index // 200))
    folder.mkdir(exist_ok=True)
    (folder / ("sample-%06d.txt" % index)).write_bytes(b"Synthetic benchmark data\n")
expected_folders = (args.files + 199) // 200
everything_binary = everything_dir / "Everything.exe"
shutil.copyfile(args.everything.resolve(), everything_binary)
instance = "DeskFlowComparison-" + uuid.uuid4().hex
configuration = {
    "app_data": 0, "run_as_admin": 0, "run_in_background": 1,
    "show_tray_icon": 0, "check_for_updates_on_startup": 0,
    "auto_include_fixed_volumes": 0, "auto_include_removable_volumes": 0,
    "auto_include_fixed_refs_volumes": 0, "auto_include_removable_refs_volumes": 0,
    "ntfs_volume_guids": "", "ntfs_volume_paths": "", "ntfs_volume_includes": "",
    "refs_volume_guids": "", "refs_volume_paths": "", "refs_volume_includes": "",
    "folders": str(root), "folder_monitor_changes": 1,
    "folder_buffer_size_list": 262144, "folder_rescan_if_full_list": 1,
    "folder_update_types": 0, "index_size": 1, "index_date_modified": 1,
    "fast_path_sort": 1, "fast_size_sort": 1, "fast_date_modified_sort": 1,
    "index_folder_size": 0, "index_date_created": 0, "index_date_accessed": 0,
    "index_recent_changes": 0, "filelists": "", "exclude_folders": "",
    "allow_http_server": 0, "allow_etp_server": 0,
}
ini = everything_dir / "Everything.ini"
ini.write_text("[Everything]\n" + "".join(f"{k}={v}\n" for k, v in configuration.items()), encoding="utf-8")
flags = subprocess.CREATE_NO_WINDOW
processes = []
handles = []
kernel = ctypes.WinDLL("kernel32", use_last_error=True)
psapi = ctypes.WinDLL("psapi", use_last_error=True)
kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
kernel.OpenProcess.restype = wintypes.HANDLE
kernel.CloseHandle.argtypes = [wintypes.HANDLE]
kernel.GetProcessTimes.argtypes = [wintypes.HANDLE, *([ctypes.POINTER(wintypes.FILETIME)] * 4)]

class Memory(ctypes.Structure):
    _fields_ = [
        ("cb", wintypes.DWORD), ("faults", wintypes.DWORD),
        ("peak_working_set", ctypes.c_size_t), ("working_set", ctypes.c_size_t),
        ("peak_paged", ctypes.c_size_t), ("paged", ctypes.c_size_t),
        ("peak_nonpaged", ctypes.c_size_t), ("nonpaged", ctypes.c_size_t),
        ("pagefile", ctypes.c_size_t), ("peak_pagefile", ctypes.c_size_t),
        ("private", ctypes.c_size_t),
    ]
psapi.GetProcessMemoryInfo.argtypes = [wintypes.HANDLE, ctypes.POINTER(Memory), wintypes.DWORD]

def sample(handle):
    memory = Memory()
    memory.cb = ctypes.sizeof(memory)
    if not psapi.GetProcessMemoryInfo(handle, ctypes.byref(memory), memory.cb):
        raise ctypes.WinError(ctypes.get_last_error())
    values = [wintypes.FILETIME() for _ in range(4)]
    if not kernel.GetProcessTimes(handle, *map(ctypes.byref, values)):
        raise ctypes.WinError(ctypes.get_last_error())
    ticks = sum((t.dwHighDateTime << 32) + t.dwLowDateTime for t in values[2:])
    return {"private_bytes": memory.private, "working_set_bytes": memory.working_set,
            "cpu_seconds": ticks / 10000000}

def count_everything(query):
    run = subprocess.run(
        [str(args.es.resolve()), "-instance", instance, "-timeout", "2000",
         "-get-result-count", query], capture_output=True, text=True,
        creationflags=flags, timeout=5,
    )
    return int(run.stdout.strip()) if run.returncode == 0 and run.stdout.strip().isdigit() else -1

def count_desk():
    path = desk_data / "files.db"
    if not path.exists():
        return None
    with sqlite3.connect(path.as_uri() + "?mode=ro", uri=True, timeout=1) as db:
        metadata = dict(db.execute("SELECT key,value FROM search_meta"))
        counts = db.execute("SELECT sum(folder=0),sum(folder=1) FROM files").fetchone()
        return metadata, counts

try:
    host = subprocess.Popen(
        [str(args.desk_directory.resolve() / "DeskFlow.exe"), "--tray",
         "--idle-test", "3600", "--data", str(desk_data)], creationflags=flags,
    )
    processes.append(host)
    worker = subprocess.Popen(
        [str(args.desk_directory.resolve() / "DeskIndex.exe"), "--data", str(desk_data),
         "--root", str(root), "--parent", str(host.pid)], creationflags=flags,
    )
    processes.append(worker)
    everything = subprocess.Popen(
        [str(everything_binary), "-instance", instance, "-config", str(ini),
         "-db", str(everything_dir / "Everything.db"), "-startup"],
        creationflags=flags,
    )
    processes.append(everything)
    # Only publish counts after both instances prove their exact coverage.
    deadline = time.monotonic() + 240
    ready = False
    while time.monotonic() < deadline:
        if any(p.poll() is not None for p in processes):
            raise RuntimeError("An isolated benchmark process exited before sampling")
        state = count_desk()
        if state and state[0].get("building") == "0" and state[1] == (args.files, expected_folders):
            if count_everything("file:") == args.files:
                folders = count_everything("folder:")
                if folders in (expected_folders, expected_folders + 1):
                    ready = True
                    break
        time.sleep(1)
    if not ready:
        raise RuntimeError("Both isolated instances did not confirm matching fixture coverage")
    print(json.dumps({"phase": "ready", "files": args.files,
                      "desk_folders": expected_folders, "everything_folders": folders}), flush=True)
    time.sleep(5)
    for process in processes:
        handle = kernel.OpenProcess(0x0410, False, process.pid)
        if not handle:
            raise ctypes.WinError(ctypes.get_last_error())
        handles.append(handle)
    rounds = []
    for index in range(args.runs):
        before = [sample(h) for h in handles]
        began = time.monotonic()
        measurements = []
        while time.monotonic() - began < args.seconds:
            time.sleep(1)
            measurements.append([sample(h) for h in handles])
        elapsed = time.monotonic() - began
        after = [sample(h) for h in handles]
        products = {}
        for name, members in (("DeskFlow", (0, 1)), ("Everything", (2,))):
            cpu = sum(after[i]["cpu_seconds"] - before[i]["cpu_seconds"] for i in members)
            private = [sum(point[i]["private_bytes"] for i in members) for point in measurements]
            working = [sum(point[i]["working_set_bytes"] for i in members) for point in measurements]
            products[name] = {
                "cpu_percent": cpu / elapsed / os.cpu_count() * 100,
                "private_mean_mib": statistics.mean(private) / 1048576,
                "private_peak_mib": max(private) / 1048576,
                "working_set_mean_mib": statistics.mean(working) / 1048576,
                "working_set_peak_mib": max(working) / 1048576,
            }
        rounds.append({"seconds": elapsed, "results": products})
        print(json.dumps({"phase": "sampled", "round": index + 1, **rounds[-1]}), flush=True)
    state = count_desk()
    assert state[1] == (args.files, expected_folders)
    assert count_everything("file:") == args.files
    report = {
        "files": args.files, "desk_folders": expected_folders,
        "everything_folders": count_everything("folder:"),
        "logical_processors": os.cpu_count(), "runs": rounds,
        "mode": "isolated_folder_index_idle_hidden_no_ocr_no_recording",
        "clipboard": "suppressed_for_synthetic_fixture",
        "binary_sha256": {
            name: hashlib.sha256(path.read_bytes()).hexdigest() for name, path in (
                ("DeskFlow", args.desk_directory / "DeskFlow.exe"),
                ("DeskIndex", args.desk_directory / "DeskIndex.exe"),
                ("Everything", args.everything),
            )
        },
    }
    (args.output / "resource-comparison.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps({"phase": "complete", "report": "resource-comparison.json"}), flush=True)
finally:
    for handle in handles:
        kernel.CloseHandle(handle)
    if len(processes) == 3 and processes[2].poll() is None:
        try:
            subprocess.run([str(everything_binary), "-instance", instance, "-config", str(ini),
                            "-exit"], creationflags=flags, timeout=10, check=False)
        except (OSError, subprocess.TimeoutExpired):
            pass  # The exact owned Popen instance is still terminated below.
    # Only terminate the exact synthetic Popen instances owned by this probe.
    for process in reversed(processes):
        if process.poll() is None:
            process.terminate()
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)
