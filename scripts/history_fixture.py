"""Create a bounded, entirely synthetic disk-backed clipboard benchmark fixture."""
from __future__ import annotations

import argparse
import ctypes
import hashlib
import json
import pathlib
import random
import shutil
import sqlite3
import struct
import time

ROOT = pathlib.Path(__file__).resolve().parents[1]
DIRECTORY = ROOT / "artifacts" / "validation" / "history-benchmark-data"
TEXT_RECORDS = 100_000
IMAGE_RECORDS = 1_024
WIDTH, HEIGHT = 1_024, 512
SEED = 20261006
OWNER = "DeskFlow synthetic history benchmark fixture v1"

SCHEMA = """
PRAGMA foreign_keys=ON;
PRAGMA cache_size=-4096;
PRAGMA mmap_size=0;
PRAGMA journal_mode=WAL;
PRAGMA synchronous=FULL;
CREATE TABLE clips(id INTEGER PRIMARY KEY,kind TEXT NOT NULL,title TEXT NOT NULL,
 body TEXT NOT NULL,source TEXT NOT NULL,created INTEGER NOT NULL,
 pinned INTEGER NOT NULL DEFAULT 0,size INTEGER NOT NULL,hash TEXT NOT NULL);
CREATE TABLE formats(clip_id INTEGER REFERENCES clips(id) ON DELETE CASCADE,
 format INTEGER NOT NULL,name TEXT NOT NULL,path TEXT NOT NULL,size INTEGER NOT NULL,
 PRIMARY KEY(clip_id,format));
CREATE INDEX clips_date ON clips(created DESC);
CREATE INDEX formats_object ON formats(path);
CREATE VIRTUAL TABLE clips_fts USING fts5(body,content='clips',content_rowid='id',tokenize='trigram');
PRAGMA user_version=1;
"""
TRIGGERS = """
CREATE TRIGGER clips_ai AFTER INSERT ON clips BEGIN
 INSERT INTO clips_fts(rowid,body) VALUES(new.id,new.body);END;
CREATE TRIGGER clips_ad AFTER DELETE ON clips BEGIN
 INSERT INTO clips_fts(clips_fts,rowid,body) VALUES('delete',old.id,old.body);END;
"""


def write_json(path: pathlib.Path, value: dict) -> None:
    temporary = path.with_suffix(path.suffix + ".pending")
    temporary.write_text(json.dumps(value, ensure_ascii=False, indent=2), encoding="utf-8")
    temporary.replace(path)


def store_object(objects: pathlib.Path, content: bytes) -> str:
    token = hashlib.sha256(content).hexdigest()
    path = objects / token
    if path.exists():
        # A repeat after interruption reuses only verified fixture objects.
        digest = hashlib.sha256()
        with path.open("rb") as stream:
            for block in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(block)
        if path.stat().st_size != len(content) or digest.hexdigest() != token:
            raise RuntimeError(f"Existing synthetic object failed verification: {token}")
    else:
        with path.open("xb") as stream:
            stream.write(content)
    return token


def allocation(path: pathlib.Path) -> int:
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    function = kernel.GetCompressedFileSizeW
    function.argtypes = [ctypes.c_wchar_p, ctypes.POINTER(ctypes.c_uint32)]
    function.restype = ctypes.c_uint32
    high = ctypes.c_uint32()
    ctypes.set_last_error(0)
    low = function(str(path), ctypes.byref(high))
    if low == 0xFFFFFFFF and ctypes.get_last_error():
        raise ctypes.WinError(ctypes.get_last_error())
    return (high.value << 32) | low


def generate() -> dict:
    directory = DIRECTORY.resolve()
    expected = (ROOT / "artifacts" / "validation" / "history-benchmark-data").resolve()
    if directory != expected:
        raise RuntimeError("Fixture target escaped the dedicated validation directory")
    marker = directory / "fixture.json"
    if marker.exists():
        previous = json.loads(marker.read_text(encoding="utf-8"))
        if previous.get("owner") != OWNER:
            raise RuntimeError("Refusing to overwrite a directory without this fixture's ownership marker")
        if previous.get("complete"):
            return previous
    elif directory.exists() and any(directory.iterdir()):
        raise RuntimeError("Refusing to overwrite pre-existing data in the fixture directory")
    directory.mkdir(parents=True, exist_ok=True)
    write_json(marker, {"owner": OWNER, "complete": False, "seed": SEED})
    if shutil.disk_usage(directory).free < 3 * 1024**3:
        raise RuntimeError("Synthetic fixture needs at least 3 GiB of free space")
    objects = directory / "objects"
    objects.mkdir(exist_ok=True)
    # Only these three owned database files are replaced during interrupted-fixture recovery.
    for filename in ("history.db", "history.db-wal", "history.db-shm"):
        path = directory / filename
        if path.parent.resolve() != expected:
            raise RuntimeError("Unsafe fixture database path")
        path.unlink(missing_ok=True)
    started = time.perf_counter()
    db = sqlite3.connect(directory / "history.db")
    db.executescript(SCHEMA)
    logical_bytes = 0
    generated_objects = 0
    base_time = 1_791_244_800_000
    topics = ("财务", "设计", "测试", "开发", "会议", "合同", "采购", "归档")
    insert_clip = "INSERT INTO clips VALUES(?,?,?,?,?,?,?,?,?)"
    insert_format = "INSERT INTO formats VALUES(?,?,?,?,?)"
    try:
        db.execute("BEGIN IMMEDIATE")
        for identifier in range(1, TEXT_RECORDS + 1):
            body = (f"历史记录 {identifier:06d}，项目资料归档；中文搜索与剪贴板内容。"
                    f" 分类={topics[identifier % len(topics)]} 日期=2026-10-06。")
            if identifier % 200 == 0:
                body += " 采购预算季度复核。"
            if identifier % 997 == 0:
                body += " literal%_\\marker。"
            if identifier == 73_421:
                body += " 独有编号073421。"
            raw = (body + "\0").encode("utf-16-le")
            token = store_object(objects, raw)
            composite = hashlib.sha256(("13" + token).encode("ascii")).hexdigest()
            db.execute(insert_clip, (identifier, "文本", body[:120], body,
                                    "SyntheticFixture.exe", base_time + identifier,
                                    int(identifier % 250 == 0), len(raw), composite))
            db.execute(insert_format, (identifier, 13, "", token, len(raw)))
            logical_bytes += len(raw)
            generated_objects += 1
            if generated_objects % 1000 == 0:
                print(f"seed objects={generated_objects:,} bytes={logical_bytes:,} "
                      f"elapsed={time.perf_counter() - started:.1f}s", flush=True)
        pixel_size = WIDTH * HEIGHT * 4
        header = struct.pack("<IiiHHIIiiII", 40, WIDTH, HEIGHT, 1, 32, 0,
                             pixel_size, 3780, 3780, 0, 0)
        for ordinal in range(1, IMAGE_RECORDS + 1):
            identifier = TEXT_RECORDS + ordinal
            # Random BGRA pixels are valid BI_RGB data and prevent filesystem compression
            # from making an apparent 2 GiB fixture occupy only a tiny amount of disk.
            pixels = random.Random(SEED + ordinal).randbytes(pixel_size)
            raw = header + pixels
            token = store_object(objects, raw)
            composite = hashlib.sha256(("8" + token).encode("ascii")).hexdigest()
            db.execute(insert_clip, (identifier, "图片", f"合成图片 {ordinal:04d}", "",
                                    "SyntheticFixture.exe", base_time + identifier,
                                    0, len(raw), composite))
            db.execute(insert_format, (identifier, 8, "", token, len(raw)))
            logical_bytes += len(raw)
            generated_objects += 1
            if generated_objects % 1000 == 0:
                print(f"seed objects={generated_objects:,} bytes={logical_bytes:,} "
                      f"elapsed={time.perf_counter() - started:.1f}s", flush=True)
        db.execute("INSERT INTO clips_fts(clips_fts) VALUES('rebuild')")
        db.commit()
        db.executescript(TRIGGERS)
        db.execute("INSERT INTO clips_fts(clips_fts) VALUES('integrity-check')")
        db.commit()
        if db.execute("PRAGMA integrity_check").fetchone()[0] != "ok":
            raise RuntimeError("Synthetic database integrity check failed")
        if db.execute("PRAGMA foreign_key_check").fetchone() is not None:
            raise RuntimeError("Synthetic database has an invalid attachment reference")
        db.execute("PRAGMA wal_checkpoint(TRUNCATE)")
    finally:
        db.close()
    actual_count, allocated_bytes = 0, 0
    for path in objects.iterdir():
        if path.is_file():
            actual_count += 1
            allocated_bytes += allocation(path)
    if actual_count != generated_objects:
        raise RuntimeError("Fixture contains unaccounted or missing objects")
    if allocated_bytes < 2 * 1024**3:
        raise RuntimeError("Fixture has less than 2 GiB of physically allocated object data")
    manifest = {
        "owner": OWNER, "complete": True, "seed": SEED,
        "directory": str(directory), "textRecords": TEXT_RECORDS,
        "imageRecords": IMAGE_RECORDS, "totalRecords": TEXT_RECORDS + IMAGE_RECORDS,
        "objectCount": actual_count, "objectBytes": logical_bytes,
        "objectAllocatedBytes": allocated_bytes,
        "imagePixelBytes": IMAGE_RECORDS * WIDTH * HEIGHT * 4,
        "imageFormat": "CF_DIB / BITMAPINFOHEADER / 1024x512 / 32bpp BI_RGB",
        "databaseBytes": (directory / "history.db").stat().st_size,
        "durationSeconds": round(time.perf_counter() - started, 3),
        "sqliteGeneratorVersion": sqlite3.sqlite_version,
        "memoryMethod": "one text or 2 MiB image payload at a time; SQLite cache 4 MiB; no mmap",
    }
    write_json(marker, manifest)
    return manifest


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.parse_args()
    print(json.dumps(generate(), ensure_ascii=False, indent=2), flush=True)


if __name__ == "__main__":
    main()
