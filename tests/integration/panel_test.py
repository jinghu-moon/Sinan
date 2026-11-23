"""Native HWND/EDIT smoke in an isolated root; no Explorer side effects."""
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

exe = Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix="sinan-panel-") as temp:
    root = Path(temp) / "unused configuration 中文"
    report = Path(temp) / "window-report.json"
    p = subprocess.run([str(exe), "--demo", "--data-dir", str(root), "--smoke-report", str(report)], timeout=20)
    assert report.is_file(), (p.returncode, "No native window report")
    result = json.loads(report.read_text(encoding="utf-8"))
    assert p.returncode == 0 and result["passed"], result
    assert all(result[key] for key in ("fluent_light", "fluent_dark", "accent_contrast", "native_edit_theme", "system_theme_refresh")), result
    assert not result["explorer_mutated"] and not result["agent_started"]
    assert not root.exists(), "Search/preview never creates configuration or agent state"
    for theme in ("light", "dark", "system"):
        p = subprocess.run([str(exe), "--demo", "--data-dir", str(root), "--theme", theme,
                            "--lang", "en", "--smoke-report", str(report)], timeout=20)
        assert p.returncode == 0 and json.loads(report.read_text(encoding="utf-8"))["passed"], theme
    for args in (("--lang", "unknown"), ("--theme", "invalid"), ("--theme",)):
        assert subprocess.run([str(exe), *args], timeout=5).returncode == 2, args
    snapshots = Path(temp) / "渲染样本"
    assert subprocess.run([str(exe), "--demo", "--data-dir", str(root), "--snapshot-dir", str(snapshots)], timeout=20).returncode == 0
    images = list(snapshots.rglob("*.png"))
    assert len(images) == 8, images
    for image in images:
        data = image.read_bytes()
        assert data[:8] == b"\x89PNG\r\n\x1a\n", image
        width, height = struct.unpack_from(">II", data, 16)
        assert width >= 864 and height >= 614, (image, width, height)
    assert not root.exists(), "Theme/render tests never write configuration or agent state"
print("Native panel edit, bilingual search, empty state, filtering, keyboard, resize and Fluent light/dark rendering passed")
