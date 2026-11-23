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
    launcher = exe.with_name("sinan-login.exe")
    def check_register(entry, expected):
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, key_path) as key:
            try:
                value = winreg.QueryValueEx(key, entry)[0]
            except FileNotFoundError:
                value = None
            assert value == expected

    try:
        initial = run("startup", "status")
        assert not initial["enabled"] and not initial["login_registered"]
        enabled = run("startup", "on")
        registered = enabled["entry"]
        assert enabled["enabled"] and enabled["login_registered"] and not enabled["workspace_restore"]
        assert run("config", "show")["autostart"] is True
        assert not run("agent", "status")["running"], "registering startup is separate from launching the Agent"
        owned = json.loads((root / "startup-state.json").read_text(encoding="utf-8"))["command"]
        check_register(registered, owned)
        subprocess.run([str(launcher), "--data-dir", str(root)], creationflags=subprocess.CREATE_NO_WINDOW, timeout=5, check=True)
        for attempt in range(30):
            state = run("agent", "status")
            if state.get("running"):
                break
            time.sleep(0.1)
        assert state["running"] and state["mode"] == "observe"
        assert Path(state["executable"]).samefile(exe.with_name("sinan-agent.exe"))
        assert run("workspace", "automatic", "on")["startup"]["entry"] == registered
        assert run("workspace", "automatic", "off")["startup"]["login_registered"]
        assert run("workspace", "automatic", "on")["startup"]["login_registered"]
        disabled = run("startup", "off")
        assert not disabled["enabled"] and disabled["workspace_restore"] and disabled["login_registered"]
        check_register(registered, owned)
        assert not run("workspace", "automatic", "off")["startup"]["login_registered"]
        check_register(registered, None)
        assert not (root / "startup-state.json").exists()
        assert run("config", "show")["restore"]["automatic"] is False
        assert run("agent", "status")["running"], "disabling sign-in startup leaves the active Agent running"
        run("agent", "stop")
        subprocess.run([str(launcher), "--data-dir", str(root)], creationflags=subprocess.CREATE_NO_WINDOW, timeout=5, check=True)
        time.sleep(0.4)
        assert not run("agent", "status")["running"], "a stale startup command must not launch an Agent"
        foreign = "another command"
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, key_path, access=winreg.KEY_SET_VALUE) as key:
            winreg.SetValueEx(key, registered, 0, winreg.REG_SZ, foreign)
            try:
                refused = subprocess.run([str(exe), "--data-dir", str(root), "startup", "on", "--json"],
                                         capture_output=True, timeout=10)
                assert refused.returncode == 5, (refused.stdout, refused.stderr)
                conflicted = run("startup", "status")
                assert conflicted["enabled"] and not conflicted["login_registered"] and conflicted["foreign_entry_preserved"]
                assert run("startup", "off")["foreign_entry_preserved"]
                check_register(registered, foreign)
            finally:
                winreg.DeleteValue(key, registered)
    finally:
        run("agent", "stop")
        run("startup", "off")
        run("workspace", "automatic", "off")
print("Startup ownership and hidden launcher passed; no startup entry retained.")
