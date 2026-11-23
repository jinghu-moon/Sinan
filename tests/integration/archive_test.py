"""Exercise real extractors against temporary archives and compare published paths."""
import json
import os
from pathlib import Path
import shutil
from struct import pack_into
import subprocess
import sys
import tempfile
from zipfile import ZipFile

driver = Path(sys.argv[1]).resolve()
tar = Path(os.environ["SystemRoot"]) / "System32" / "tar.exe"
seven = os.environ.get("SINAN_TEST_7ZIP") or shutil.which("7z.exe")
rar = os.environ.get("SINAN_TEST_RAR") or shutil.which("rar.exe")


def tool(kind, path):
    return {"id": kind, "type": kind, "name": kind, "path": str(path)}


def run(*args, cwd=None):
    return subprocess.run(args, cwd=cwd, capture_output=True, timeout=30, check=True)


def make_split_zip(directory, payload):
    """Create a ZIP whose local record starts on disk 0 and central directory on disk 1."""
    archive = directory / "numbered.zip"
    with ZipFile(archive, "w") as output:
        output.writestr("payload.bin", payload)
    raw = archive.read_bytes()
    central = raw.rindex(b"PK\x01\x02")
    end = raw.rindex(b"PK\x05\x06")
    split = 4092
    marker = b"PK\x07\x08"
    central_record = bytearray(raw[central:end])
    pack_into("<I", central_record, 42, len(marker))
    end_record = bytearray(raw[end:])
    pack_into("<HH", end_record, 4, 1, 1)
    pack_into("<I", end_record, 16, central - split)
    (directory / "numbered.z01").write_bytes(marker + raw[:split])
    archive.write_bytes(raw[split:central] + central_record + end_record)
    return archive


with tempfile.TemporaryDirectory(prefix="sinan-archive-") as temporary:
    root = Path(temporary)
    request = root / "request.json"
    settings = {"tools": [tool("tar", tar)], "default_tool": "tar", "formats": {}, "recycle": False}

    def extract(*paths, expected=0, cancel=False):
        request.write_text(json.dumps({"selected": list(map(str, paths)), "settings": settings,
                                       "cancel": cancel}), encoding="utf-8")
        proc = subprocess.run([driver, request], capture_output=True, timeout=30)
        output = json.loads(proc.stdout)
        if expected is None:
            assert proc.returncode != 0, output
        else:
            assert proc.returncode == expected, output
        assert not list(root.glob(".sinan-extract-*")), list(root.iterdir())
        return output

    archive = root / "different name.ZIP"
    with ZipFile(archive, "w") as output:
        output.writestr("项目资料/文档/说明.txt", "readme")
    result = extract(archive)
    assert Path(result["output"]) == root / "项目资料", result
    assert Path(result["directory"]) == root / "项目资料" / "文档", result
    assert Path(result["file"]).read_text() == "readme"
    assert archive.exists()
    assert Path(extract(archive)["output"]) == root / "项目资料 (2)"
    assert "canceled" in extract(archive, expected=7, cancel=True)["error"]

    flat = root / "mixed.zip"
    with ZipFile(flat, "w") as output:
        output.writestr("a.txt", "a")
        output.writestr("b/c.txt", "bc")
    result = extract(flat)
    assert result["directory"] == result["output"] == str(root / "mixed")
    assert result["file"] == ""

    lone = root / "lone.zip"
    with ZipFile(lone, "w") as output:
        output.writestr("single.txt", "only file")
    assert Path(extract(lone)["file"]) == root / "lone" / "single.txt"
    assert "Select one" in extract(lone, flat, expected=2)["error"]

    empty = root / "empty.zip"
    with ZipFile(empty, "w"):
        pass
    assert "empty" in extract(empty, expected=2)["error"].lower()
    broken = root / "broken.zip"
    broken.write_bytes(b"invalid archive")
    extract(broken, expected=None)
    assert broken.exists() and not (root / "broken").exists()

    unsafe = root / "unsafe.zip"
    with ZipFile(unsafe, "w") as output:
        output.writestr("../unexpected.txt", "test")
    extract(unsafe, expected=None)
    assert not (root / "unexpected.txt").exists()

    staging = root / "input"
    staging.mkdir()
    payload = staging / "payload.bin"
    payload.write_bytes(os.urandom(19000))
    if seven:
        settings["tools"].append(tool("7zip", seven))
        settings["formats"][".7Z"] = "7zip"
        run(seven, "a", "-v4k", root / "volumes.7z", payload.name, cwd=staging)
        middle = root / "volumes.7z.002"
        first = root / "volumes.7z.001"
        assert Path(extract(middle, first)["file"]).read_bytes() == payload.read_bytes()
        hidden = root / "hold.7z.002"
        middle.rename(hidden)
        assert "Missing" in extract(first, expected=2)["error"]
        hidden.rename(middle)
        last = sorted(root.glob("volumes.7z.*"))[-1]
        hold = last.with_name("hold.7z.999")
        last.rename(hold)
        extract(first, expected=None)
        assert not (root / "volumes (2)").exists()
        last.write_bytes(os.urandom(hold.stat().st_size))
        extract(first, expected=None)
        last.unlink()
        hold.rename(last)
        settings["formats"][".zip"] = "7zip"
        run(seven, "a", "-tzip", "-v4k", root / "split.zip", payload.name, cwd=staging)
        assert Path(extract(root / "split.zip.002")["file"]).read_bytes() == payload.read_bytes()
        disk_zip = make_split_zip(root, payload.read_bytes())
        first_disk = root / "numbered.z01"
        assert Path(extract(first_disk, disk_zip)["file"]).read_bytes() == payload.read_bytes()
        first_disk.rename(root / "hold.z01")
        extract(disk_zip, expected=None)
        assert not (root / "numbered (2)").exists()

    if rar:
        settings["tools"].append(tool("rar", rar))
        settings["formats"][".rar"] = "rar"
        run(rar, "a", "-v4k", root / "parts.rar", payload.name, cwd=staging)
        middle = next(root.glob("parts.part*2.rar"))
        assert Path(extract(middle)["file"]).read_bytes() == payload.read_bytes()
        # Recent versions of the RAR creator may provide RAR5 output only.
        legacy = subprocess.run([rar, "a", "-ma4", "-vn", "-v4k", root / "legacy.rar", payload.name],
                                cwd=staging, capture_output=True, timeout=30)
        if legacy.returncode == 0 and (root / "legacy.r00").exists():
            assert Path(extract(root / "legacy.r00")["file"]).read_bytes() == payload.read_bytes()

print(f"Archive extraction and failure preservation passed. 7-Zip={bool(seven)} RAR={bool(rar)}")
