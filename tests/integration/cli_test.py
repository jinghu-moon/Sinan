"""Exercise the real CLI with isolated config; never install, inject, or restart Explorer."""
import concurrent.futures
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import tomllib

exe = str(Path(sys.argv[1]).resolve())
with tempfile.TemporaryDirectory(prefix="Sinan-cli-") as directory:
    root = Path(directory) / "中文路径 with spaces"

    def run(*args, code=0):
        result = subprocess.run([exe, "--data-dir", str(root), *args], input="", capture_output=True,
                                text=True, encoding="utf-8", timeout=15)
        assert result.returncode == code, (args, result.returncode, result.stdout, result.stderr)
        return result

    assert "Sinan" in run("--version").stdout
    assert "config" in run("--help").stdout
    assert not root.exists(), "Help/version must not create config"
    assert run("preview", r"D:\A\B\C").stdout.strip() == r"B\C"
    assert run("preview", r"D:\A\B\C", "--depth", "all").stdout.strip() == r"D:\A\B\C"
    assert run("preview", r"D:\项目\语音输入", "--separator", " · ").stdout.strip() == "项目 · 语音输入"
    run("preview", r"D:\A\B", "--separator", "invalid", code=2)
    run("preview", r"D:\A\B", "--depth", "0", code=2)
    preview = json.loads(run("preview", r"\\nas\share\A\B", "--json").stdout)
    assert preview["title"] == r"A\B" and preview["filesystem"]
    run("config", "set", "depth", "3")
    config_file = root / "sinan.toml"
    config = tomllib.loads(config_file.read_text(encoding="utf-8"))
    assert config["display"]["depth"] == 3
    assert config["enabled"] is False
    saved = config_file.read_bytes()
    run("config", "set", "depth", "0", code=2)
    run("config", "set", "separator", "invalid", code=2)
    run("config", "set", "unknown", "2", code=2)
    assert config_file.read_bytes() == saved
    run("config", "set", "depth", "all")
    assert json.loads(run("config", "show", "--json").stdout)["display"]["depth"] == "all"
    before_revision = tomllib.loads(config_file.read_text(encoding="utf-8"))["revision"]
    with concurrent.futures.ThreadPoolExecutor(max_workers=6) as pool:
        list(pool.map(lambda _: run("config", "set", "depth", "2"), range(12)))
    assert tomllib.loads(config_file.read_text(encoding="utf-8"))["revision"] == before_revision + 12
    run("config", "reset")
    assert json.loads(run("config", "show", "--json").stdout)["display"] == {"depth": 2, "separator": "\\"}
    status = json.loads(run("status", "--json").stdout)
    assert status["hook_available"] is False and status["runtime_applied"] is False
    run("appearance", "set", "fixed", "--width", "360")
    assert json.loads(run("appearance", "show").stdout)["width"] == 360
    run("appearance", "set", "content", "--min", "140", "--max", "420")
    appearance = json.loads(run("config", "show", "--json").stdout)["appearance"]["tabs"]
    assert appearance == {"width_mode": "content", "width": 360, "min_width": 140, "max_width": 420}
    saved = config_file.read_bytes()
    run("appearance", "set", "content", "--min", "500", "--max", "200", code=2)
    run("appearance", "set", "system", "--width", "320", code=2)
    run("appearance", "set", "fixed", "--width", "3.5", code=2)
    run("appearance", "set", "fixed", "--width", "320.5", code=2)
    assert config_file.read_bytes() == saved
    assert json.loads(run("appearance", "status").stdout)["layout_applied"] is False
    run("appearance", "reset")
    assert json.loads(run("appearance", "show").stdout)["width_mode"] == "system"
    assert json.loads(run("config", "show", "--json").stdout)["display"] == {"depth": 2, "separator": "\\"}
    assert json.loads(run("shortcuts", "show").stdout)["toggle_pin"] == ""
    run("shortcuts", "set", "toggle_pin", "Ctrl+Alt+P")
    assert json.loads(run("shortcuts", "show").stdout)["toggle_pin"] == "Ctrl+Alt+P"
    before = config_file.read_bytes()
    run("shortcuts", "set", "toggle_protection", "Ctrl+Alt+P", code=2)
    run("shortcuts", "set", "toggle_pin", "Ctrl+Shift+W", code=2)
    assert config_file.read_bytes() == before
    style_file = root / "style.json"
    styles = json.loads(run("style", "show").stdout)
    styles["default"] = {"text": {"font_color": "#123456", "bold": False}, "dark": {"background": {"selected": "#334455"}}}
    style_file.write_text(json.dumps(styles), encoding="utf-8")
    run("style", "load", str(style_file))
    assert json.loads(run("style", "show").stdout) == styles
    assert tomllib.loads(config_file.read_text(encoding="utf-8"))["schema_version"] == 4
    before = config_file.read_bytes()
    styles["default"]["text"]["font_family"] = "NoSuchSinanFont9827"
    style_file.write_text(json.dumps(styles), encoding="utf-8")
    run("style", "load", str(style_file), code=2)
    assert config_file.read_bytes() == before
    assert "automatic" in run("workspace", "--help").stdout
    assert "batch" in run("tabs", "--help").stdout
    doctor = json.loads(run("doctor", "--json").stdout)
    assert doctor["config_valid"] and doctor["windows_build"] != "unknown"
    assert isinstance(doctor["runtime_available"], bool)
    assert len(doctor["explorerframe_sha256"]) == 64
    run("explorer", "restart", code=3)  # No --force: must never touch the shell.
    run("explorer", "restart", "--force", code=6)  # No --yes and stdin isn't a terminal.
    run("install", code=2)  # --data-dir may never redirect real installation/registry writes.
    run("path", "add", code=2)
    assert "start" in run("--help").stdout
    config_file.write_text("schema_version = 999\n", encoding="utf-8")
    broken = config_file.read_bytes()
    run("config", "show", code=2)
    run("config", "set", "depth", "2", code=2)
    assert config_file.read_bytes() == broken
    config_file.write_text('schema_version = 1\nrevision = 1\nenabled = false\nautostart = false\n[display]\ndepth = 2.0\nseparator = "\\\\"\n', encoding="utf-8")
    run("config", "show", code=2)
    config_file.write_text("[bad TOML\n", encoding="utf-8")
    run("config", "show", code=2)
print("CLI integration passed (including 12 concurrent config writers)")
