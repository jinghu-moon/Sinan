"""Directory entry CRUD, alias previews, migration and docking config; no Explorer writes."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import tomllib

exe = str(Path(sys.argv[1]).resolve())
with tempfile.TemporaryDirectory(prefix="Sinan-entries-") as temporary:
    root = Path(temporary)
    def call(*args, code=0):
        p = subprocess.run([exe, "--data-dir", str(root), *args, "--json"], capture_output=True,
                           encoding="utf-8", timeout=20)
        assert p.returncode == code, (args, p.returncode, p.stdout, p.stderr)
        return json.loads(p.stdout) if p.stdout.strip() else {}

    call("entries", "add", "work", r"D:\Projects\Sinan", "--name", "开发", "--descendants", "--shortcut", "Ctrl+Alt+1")
    assert call("entries", "list")[0]["name"] == "开发"
    assert call("rules", "test", r"D:\Projects\Sinan")["preview_title"] == "开发"
    assert call("rules", "test", r"D:\Projects\Sinan\src\agent\config")["preview_title"] == "开发 · agent\\config"
    call("entries", "add", "work", r"D:\Projects\Other", "--name", "冲突", code=2)
    call("shortcuts", "set", "toggle_pin", "Alt+Ctrl+1", code=2)
    call("entries", "add", "relative", "relative/path", "--name", "错误", code=2)
    call("entries", "add", "child", r"D:\Projects\Sinan\src", "--name", "源码", "--title", "alias", "--descendants", "--scope", "explorer")
    assert call("rules", "test", r"D:\Projects\Sinan\src\agent")["preview_title"] == "源码"
    call("entries", "add", "child", r"D:\Projects\Sinan\src", "--name", "源码", "--title", "off", "--descendants", "--replace")
    assert next(e for e in call("entries", "list") if e["id"] == "child")["scope"] == "explorer"
    assert call("rules", "test", r"D:\Projects\Sinan\src\agent")["preview_title"] == r"src\agent"
    for side in ("left", "right", "keep"):
        call("pinned", "position", side)
        assert call("pinned", "show")["pinned_position"] == side
    call("pinned", "position", "middle", code=2)
    call("entries", "remove", "child")
    assert len(call("entries", "list")) == 1
    call("entries", "remove", "child", code=2)
    call("entries", "open", "work", code=3)  # Agent required, no implicit Explorer launch.
    disk = tomllib.loads((root / "sinan.toml").read_text(encoding="utf-8"))
    assert disk["schema_version"] == 4 and disk["directories"]["entries"][0]["shortcut"] == "Ctrl+Alt+1"
    legacy = "schema_version=3\nrevision=9\nenabled=false\nautostart=false\n[display]\ndepth=2\nseparator='\\'\n"
    (root / "sinan.toml").write_text(legacy, encoding="utf-8")
    original = (root / "sinan.toml").read_bytes()
    call("config", "show")
    assert not (root / "sinan.pre-schema4.toml").exists()
    call("entries", "add", "migration", r"D:\Migration", "--name", "迁移")
    assert (root / "sinan.pre-schema4.toml").read_bytes() == original
    created = call("actions", "run", "explorer.new_folder", "--parameters",
                   json.dumps({"parent": str(root), "name": "新的 文件夹"}, ensure_ascii=False))
    assert created["created"] and (root / "新的 文件夹").is_dir()
    call("actions", "run", "explorer.new_folder", "--parameters",
         json.dumps({"parent": str(root), "name": "新的 文件夹"}, ensure_ascii=False), code=2)
    call("actions", "run", "directories.open", "--parameters", '{"id":"migration"}', code=3)
    call("actions", "run", "explorer.copy_selection", code=3)
    assert call("scripts", "list") == []
    script = root / "测试 script.ps1"
    call("scripts", "add", "relative", "relative.ps1", "--name", "Invalid", code=2)
    call("scripts", "add", "type", str(script.with_suffix(".cmd")), "--name", "Invalid", code=2)
    call("scripts", "add", "context", str(script), "--name", "记录上下文", "--scope", "explorer",
         "--shortcut", "Ctrl+Alt+Shift+F12")
    saved = call("scripts", "list")
    assert saved[0]["path"] == str(script) and saved[0]["scope"] == "explorer"
    assert tomllib.loads((root / "sinan.toml").read_text(encoding="utf-8"))["scripts"]["entries"] == saved
    call("scripts", "add", "context", str(script), "--name", "Duplicate", code=2)
    call("entries", "add", "conflict", r"D:\ScriptConflict", "--name", "Conflict",
         "--shortcut", "Alt+Shift+Ctrl+F12", code=2)
    call("shortcuts", "set", "copy_selection", "Ctrl+Alt+Shift+F12", code=2)
    rows = call("actions", "list", "--query", "记录上下文")["actions"]
    assert len(rows) == 1 and rows[0]["id"] == "script.context" and rows[0]["shortcut_scopes"] == ["explorer"]
    call("actions", "run", "script.context", code=3)
    call("scripts", "add", "context", str(script), "--name", "Replaced", "--replace")
    assert call("scripts", "list")[0]["scope"] == "global"
    call("scripts", "remove", "context")
    assert call("actions", "list", "--query", "script.context")["count"] == 0
print("directory entries CLI checks passed")
