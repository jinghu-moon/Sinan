"""Verify login registration ownership and hidden launcher using an isolated root.
No Explorer injection, navigation, restart or real login is performed.
"""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import winreg

exe = Path(sys.argv[1]).resolve()
key_path = r"Software\Microsoft\Windows\CurrentVersion\Run"
with tempfile.TemporaryDirectory(prefix="Sinan-startup-") as folder:
    root = Path(folder) / "startup root 中文"
    def run(*args):
        p = subprocess.run([str(exe), "--data-dir", str(root), *args, "--json"], capture_output=True, text=True, encoding="utf-8", timeout=15, input="")
        assert p.returncode == 0, (args, p.stdout, p.stderr)
        return json.loads(p.stdout)
    registered = None
    try:
        run("config", "set", "depth", "2")
        enabled = run("workspace", "automatic", "on")
        registered = enabled["startup"]["entry"]
        owned = json.loads((root / "startup-state.json").read_text(encoding="utf-8"))["command"]
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, key_path) as key:
            assert winreg.QueryValueEx(key, registered)[0] == owned
        subprocess.run([str(exe.with_name("sinan-login.exe")), "--data-dir", str(root)], creationflags=subprocess.CREATE_NO_WINDOW, timeout=5, check=True)
        for attempt in range(30):
            state = run("agent", "status")
            if state.get("running"):
                break
            time.sleep(0.1)
        assert state["running"] and state["mode"] == "observe"
        assert Path(state["executable"]).samefile(exe.with_name("sinan-agent.exe"))
        run("workspace", "automatic", "off")
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, key_path) as key:
            try:
                winreg.QueryValueEx(key, registered)
                raise AssertionError("owned startup entry was not removed")
            except FileNotFoundError:
                pass
        assert not (root / "startup-state.json").exists()
        assert run("config", "show")["restore"]["automatic"] is False
    finally:
        run("agent", "stop")
        run("workspace", "automatic", "off")
print("Startup ownership and hidden launcher passed; no startup entry retained.")
