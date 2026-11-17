"""Check an installed directory, never register it or change the user's PATH."""
from pathlib import Path
import json
import subprocess
import sys
import tempfile

package = Path(sys.argv[1]).resolve()
tui = len(sys.argv) < 3 or sys.argv[2] == "ON"
required = {
    "sinan.exe", "sinan-agent.exe", "sinan-login.exe", "sinan-symbols.exe",
    "sinan-hook.dll", "sinan-layout.dll", "sinan-features.dll", "sinan-style.dll",
}
assert {p.name for p in (package / "bin").iterdir()} == required
for name in ("README.md", "LICENSE", "THIRD_PARTY.md", "docs/06-sinan-refactor-tdd.md", "tools/update-agent.ps1"):
    assert (package / name).is_file(), name
licenses = {"CLI11-LICENSE", "json-LICENSE", "tomlplusplus-LICENSE", "MinHook-LICENSE"}
if tui:
    licenses.add("FTXUI-LICENSE")
assert {p.name for p in (package / "licenses").iterdir()} == licenses
for path in package.rglob("*"):
    assert path.name not in {"refer", ".serena", ".git", "tabtail.exe", "tabtail.toml"}, path
    if path.parent.name == "evidence":
        assert not path.name.startswith(("01-", "02-", "03-")), path
exe = package / "bin/sinan.exe"
with tempfile.TemporaryDirectory(prefix="sinan-package-") as temp:
    root = Path(temp) / "state"
    def run(*args):
        p = subprocess.run([str(exe), "--data-dir", str(root), *args], capture_output=True, encoding="utf-8", timeout=15)
        assert p.returncode == 0, (args, p.stdout, p.stderr)
        return p.stdout
    assert "Sinan 0.12.0" in run("--version")
    assert "actions" in run("--help")
    assert run("preview", r"D:\A\B", "--depth", "1").strip() == "B"
    assert any(a["id"] == "tabs.pin" for a in json.loads(run("actions", "list"))["actions"])
    assert not root.exists()
print("Installed manifest, notices and local commands passed")
