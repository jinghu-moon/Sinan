"""Persistent rules, explanations, reload and language; no Explorer title writes."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import tomllib

exe = str(Path(sys.argv[1]).resolve())
with tempfile.TemporaryDirectory(prefix="TabTail-rules-cli-") as temporary:
    root = Path(temporary) / "中文规则"

    def call(*arguments, code=0):
        p = subprocess.run([exe, "--data-dir", str(root), *arguments, "--json"], input="",
                           capture_output=True, text=True, encoding="utf-8", timeout=30)
        assert p.returncode == code, (arguments, p.returncode, p.stdout, p.stderr)
        return json.loads(p.stdout) if p.stdout.strip() else {}

    call("rules", "add", "src", "--name", "源代码", "--pattern", r"D:\Projects\**\{src,docs}", "--depth", "3")
    call("rules", "add", "projects", "--pattern", r"D:\Projects\**", "--depth", "1")
    result = call("rules", "test", r"D:\Projects\A\src")
    assert result["rule_id"] == "src" and result["depth"] == 3 and result["preview_title"] == r"Projects\A\src"
    call("rules", "move", "projects", "1")
    assert call("rules", "test", r"D:\Projects\A\src")["rule_id"] == "projects"
    call("rules", "toggle", "projects")
    assert call("rules", "test", r"D:\Projects\A\src")["rule_id"] == "src"
    call("rules", "add", "raw", "--pattern", r"D:\Projects\**\cache\**", "--native")
    result = call("rules", "test", r"D:\Projects\A\cache")
    assert result["action"] == "native" and result["preview_title"] is None
    call("rules", "add", "special", "--exact", "--pattern", r"D:\Projects\[literal]", "--depth", "all")
    assert call("rules", "test", r"D:\Projects\[literal]")["depth"] == "all"
    config_file = root / "tabtail.toml"
    previous = config_file.read_bytes()
    for bad in [r"!D:\Projects\**", r"D:\Projects\[!abc]", r"D:\Projects\[^abc]", r"D:\Projects\x**y"]:
        call("rules", "add", "bad", "--pattern", bad, code=2)
        assert config_file.read_bytes() == previous
    call("rules", "add", "src", "--pattern", r"D:\Other", code=2)
    call("rules", "add", "src", "--replace", "--pattern", r"D:\Projects\**\src", "--depth", "4")
    assert call("rules", "test", r"D:\Projects\A\src")["depth"] == 4
    call("rules", "delete", "special")
    assert len(call("rules", "list")) == 3
    before = config_file.read_bytes()
    for lang, word in [("en", "Directory rules"), ("zh-CN", "目录规则")]:
        p = subprocess.run([exe, "--data-dir", str(root), "tui", "--snapshot", "--lang", lang],
                           capture_output=True, text=True, encoding="utf-8", timeout=15)
        assert p.returncode == 0 and word in p.stdout, (p.returncode, p.stdout, p.stderr)
    assert config_file.read_bytes() == before, "session language override never rewrites config"
    call("tui", "--lang", "de", "--snapshot", code=2)
    call("tui", code=2)  # Redirected stdin must not start an interactive loop.
    try:
        call("agent", "start")
        listing = call("tabs", "list")
        paths = [t for t in listing.get("tabs", []) if t["path"]]
        if listing["scan_complete"] and paths:
            target = paths[0]
            call("rules", "add", "actual", "--exact", "--pattern", target["path"], "--depth", "4", code=5)
            current = {t["tab_id"]: t for t in call("tabs", "list")["tabs"]}[target["tab_id"]]
            assert current["source"] == "rule" and current["rule_id"] == "actual" and current["depth"] == 4
            call("tabs", "set", target["tab_id"], "--depth", "1", code=5)
            current = {t["tab_id"]: t for t in call("tabs", "list")["tabs"]}[target["tab_id"]]
            assert current["source"] == "tab" and current["depth"] == 1
            result = call("tabs", "reset", target["tab_id"], code=5)
            assert result["source"] == "rule" and result["depth"] == 4
            call("agent", "stop")
            call("agent", "start")
            same_path = [t for t in call("tabs", "list")["tabs"] if t["path"] == target["path"]]
            assert all(t["source"] == "rule" and t["depth"] == 4 for t in same_path)
        assert tomllib.loads(config_file.read_text(encoding="utf-8"))["schema_version"] == 4
    finally:
        call("agent", "stop")
print("Rules CLI and observation integration passed; no real titles changed.")
