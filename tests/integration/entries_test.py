"""Directory entry CRUD, alias previews, migration and docking config; no Explorer writes."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import tomllib

exe = str(Path(sys.argv[1]).resolve())
with tempfile.TemporaryDirectory(prefix="TabTail-entries-") as temporary:
    root = Path(temporary)
    def call(*args, code=0):
        p = subprocess.run([exe, "--data-dir", str(root), *args, "--json"], capture_output=True,
                           encoding="utf-8", timeout=20)
        assert p.returncode == code, (args, p.returncode, p.stdout, p.stderr)
        return json.loads(p.stdout) if p.stdout.strip() else {}

    call("entries", "add", "work", r"D:\Projects\TabTail", "--name", "开发", "--descendants", "--shortcut", "Ctrl+Alt+1")
    assert call("entries", "list")[0]["name"] == "开发"
    assert call("rules", "test", r"D:\Projects\TabTail")["preview_title"] == "开发"
    assert call("rules", "test", r"D:\Projects\TabTail\src\agent\config")["preview_title"] == "开发 · agent\\config"
    call("entries", "add", "work", r"D:\Projects\Other", "--name", "冲突", code=2)
    call("shortcuts", "set", "toggle_pin", "Alt+Ctrl+1", code=2)
    call("entries", "add", "relative", "relative/path", "--name", "错误", code=2)
    call("entries", "add", "child", r"D:\Projects\TabTail\src", "--name", "源码", "--title", "alias", "--descendants")
    assert call("rules", "test", r"D:\Projects\TabTail\src\agent")["preview_title"] == "源码"
    call("entries", "add", "child", r"D:\Projects\TabTail\src", "--name", "源码", "--title", "off", "--descendants", "--replace")
    assert call("rules", "test", r"D:\Projects\TabTail\src\agent")["preview_title"] == r"src\agent"
    for side in ("left", "right", "keep"):
        call("pinned", "position", side)
        assert call("pinned", "show")["pinned_position"] == side
    call("pinned", "position", "middle", code=2)
    call("entries", "remove", "child")
    assert len(call("entries", "list")) == 1
    call("entries", "remove", "child", code=2)
    call("entries", "open", "work", code=3)  # Agent required, no implicit Explorer launch.
    disk = tomllib.loads((root / "tabtail.toml").read_text(encoding="utf-8"))
    assert disk["schema_version"] == 4 and disk["directories"]["entries"][0]["shortcut"] == "Ctrl+Alt+1"
    legacy = "schema_version=3\nrevision=9\nenabled=false\nautostart=false\n[display]\ndepth=2\nseparator='\\'\n"
    (root / "tabtail.toml").write_text(legacy, encoding="utf-8")
    original = (root / "tabtail.toml").read_bytes()
    call("config", "show")
    assert not (root / "tabtail.pre-schema4.toml").exists()
    call("entries", "add", "migration", r"D:\Migration", "--name", "迁移")
    assert (root / "tabtail.pre-schema4.toml").read_bytes() == original
print("directory entries CLI checks passed")
