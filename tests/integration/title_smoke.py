"""Opt-in live Explorer test. Copies binaries because the hook DLL stays loaded.

python tests/integration/title_smoke.py build/msvc/Release/tabtail.exe --apply
Does not navigate, close tabs, restart Explorer, install, or change user PATH.
"""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import time
import uuid

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("executable", type=Path)
parser.add_argument("--apply", action="store_true", required=True,
                    help="temporarily change real Explorer titles and restore in finally")
args = parser.parse_args()
source = args.executable.resolve()
directory = Path(__file__).resolve().parents[2] / "build" / "title-smoke" / uuid.uuid4().hex
directory.mkdir(parents=True)
for name in ("tabtail.exe", "tabtail-hook.dll"):
    shutil.copy2(source.parent / name, directory / name)
exe = directory / "tabtail.exe"
root = directory / "data"


def call(*arguments, code=0):
    result = subprocess.run([str(exe), "--data-dir", str(root), *arguments, "--json"],
                            input="", capture_output=True, text=True, encoding="utf-8", timeout=30)
    assert result.returncode == code, (arguments, result.returncode, result.stdout, result.stderr)
    return json.loads(result.stdout)


report = {"directory": str(directory), "visual_verification": False}
try:
    call("agent", "start")
    initial = call("tabs", "list")
    assert initial["hook_available"], "unsupported ExplorerFrame; no titles changed"
    targets = [t for t in initial["tabs"] if t["path"] and t["native_guid"]]
    assert initial["scan_complete"] and len(targets) >= 2, "need two filesystem tabs"
    first, second = targets[:2]
    for tab in initial["tabs"]:
        assert call("tabs", "probe", tab["tab_id"])["ui_thread_verified"]
    call("tabs", "set", first["tab_id"], "--depth", "1", code=5)
    call("tabs", "set", second["tab_id"], "--depth", "3", code=5)
    started = call("start")
    assert started["runtime_applied"] and started["mode"] == "customize", started
    listing = call("tabs", "list")["tabs"]
    for tab in listing:
        assert tab["title_applied"] and tab["applied_title"] == tab["preview_title"], tab
    by_id = {t["tab_id"]: t for t in listing}
    assert by_id[first["tab_id"]]["depth"] == 1
    assert by_id[second["tab_id"]]["depth"] == 3
    report["independent_titles"] = [by_id[t["tab_id"]]["applied_title"] for t in (first, second)]
    assert call("config", "set", "depth", "all")["runtime_applied"]
    changed = {t["tab_id"]: t for t in call("tabs", "list")["tabs"]}
    assert changed[first["tab_id"]]["depth"] == 1 and changed[second["tab_id"]]["depth"] == 3
    reset = call("tabs", "reset", second["tab_id"])
    assert reset["title_applied"] and reset["source"] == "global" and reset["depth"] == "all"
    time.sleep(3)  # One bounded lease-renewal interval.
    assert call("agent", "status")["runtime_applied"]
    stop = call("stop")
    assert stop["application"]["restored_count"] == len(listing), stop
    assert all(not t["title_applied"] for t in call("tabs", "list")["tabs"])
    report["restored_count"] = stop["application"]["restored_count"]
    report["result"] = "passed: native UI-thread calls acknowledged; visual check still required"
finally:
    try:
        call("stop")
    finally:
        call("agent", "stop")
    (directory / "report.json").write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding="utf-8")
print(json.dumps(report, indent=2, ensure_ascii=True))
