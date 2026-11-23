"""Use actual Explorer COM observations without navigation, injection, or title writes."""
import concurrent.futures
import ctypes
import json
from pathlib import Path
import subprocess
import sys
import tempfile

exe = str(Path(sys.argv[1]).resolve())
with tempfile.TemporaryDirectory(prefix="Sinan-agent-") as temporary:
    root = str(Path(temporary) / "中文 agent root")

    def call(*args, code=0):
        p = subprocess.run([exe, "--data-dir", root, *args, "--json"], capture_output=True,
                           text=True, encoding="utf-8", timeout=25, input="")
        assert p.returncode == code, (args, p.returncode, p.stdout, p.stderr)
        return json.loads(p.stdout) if p.stdout.strip() else {}

    try:
        call("tabs", "list", code=3)
        call("tabs", "list", "--query", "src", code=3)
        start = call("agent", "start")
        assert start["mode"] == "observe" and isinstance(start["hook_available"], bool)
        assert start["shortcuts"]["hook_active"] is False, "observation must not install a global keyboard hook"
        assert start["tray"]["registered"] is False
        assert call("agent", "start")["pid"] == start["pid"], "start must be idempotent"
        old_runtime = start["runtime_id"]
        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
            statuses = list(pool.map(lambda _: call("agent", "status"), range(16)))
        assert all(s["runtime_id"] == old_runtime for s in statuses)
        result = call("tabs", "list")
        assert "tabs" in result and result["runtime_applied"] is False
        missing = "sinan-search-no-match-8afd5fd35f6b"
        filtered = call("tabs", "list", "--query", missing)
        assert filtered["tabs"] == [] and filtered["match_count"] == 0
        assert all(key in filtered for key in ("scan_complete", "warnings", "tab_count"))
        action = call("actions", "run", "tabs.list", "--parameters", json.dumps({"query": missing}))
        assert action["tabs"] == [] and action["match_count"] == 0
        tabs = result["tabs"]
        target = None
        if result["scan_complete"] and tabs:
            target = tabs[0]["tab_id"]
            saved = call("tabs", "set", target, "--depth", "1", code=5)
            assert saved["policy_saved"] and saved["title_applied"] is False
            listing = {t["tab_id"]: t for t in call("tabs", "list")["tabs"]}
            assert listing[target]["source"] == "tab" and listing[target]["depth"] == 1
            if len(tabs) > 1:
                other = tabs[1]["tab_id"]
                call("tabs", "set", other, "--depth", "all", code=5)
                listing = {t["tab_id"]: t for t in call("tabs", "list")["tabs"]}
                assert listing[other]["depth"] == "all" and listing[other]["source"] == "tab"
            call("config", "set", "depth", "3", code=5)
            listing = {t["tab_id"]: t for t in call("tabs", "list")["tabs"]}
            assert listing[target]["depth"] == 1, "global change must not overwrite tab override"
            call("tabs", "reset", target, code=5)
            listing = {t["tab_id"]: t for t in call("tabs", "list")["tabs"]}
            assert listing[target]["source"] == "global" and listing[target]["depth"] == 3
            assert all(t["title_applied"] is False for t in listing.values())
            assert len({(t["pid"], t["browser_window"]) for t in listing.values()}) == len(listing)
        call("tabs", "set", "bad-token", "--depth", "1", code=2)
        call("tabs", "set", old_runtime + ".999999999", "--depth", "2", code=2)
        call("tabs", "set", old_runtime + ".1", "--depth", "0", code=2)
        call("agent", "stop")
        assert call("agent", "status")["running"] is False
        call("agent", "stop")
        new = call("agent", "start")
        assert new["runtime_id"] != old_runtime
        call("tabs", "locate", old_runtime + ".1", code=2)
        if target:
            call("tabs", "set", target, "--depth", "2", code=2)
        user32 = ctypes.windll.user32
        user32.FindWindowW.restype = ctypes.c_void_p
        if user32.FindWindowW("Shell_TrayWnd", None):
            assert call("tray", "on")["registered"]
            tray = call("tray", "status")
            assert tray["enabled"] and tray["registered"] and tray["running"]
            assert call("tray", "off")["registered"] is False
            assert call("agent", "status")["pid"] == new["pid"]
        call("agent", "stop")
        assert call("tray", "status")["enabled"] is False
        assert call("tray", "on")["saved"]
        tray = call("tray", "status")
        assert tray["enabled"] and not tray["registered"] and not tray["running"]
        assert call("startup", "status")["enabled"] is False
        print(f"Agent integration passed; {len(tabs)} real tabs observed, none modified.")
    finally:
        call("agent", "stop")
