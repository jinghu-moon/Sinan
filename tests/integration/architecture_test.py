"""Public product/process contract; isolated, no Explorer writes."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile

exe = Path(sys.argv[1]).resolve()
assert exe.name == "sinan.exe", "CLI must be the renamed Sinan product"
agent = exe.with_name("sinan-agent.exe")
assert agent.is_file(), "Agent must have a separate executable"
with tempfile.TemporaryDirectory(prefix="sinan-architecture-") as temp:
    root = Path(temp) / "state 中文"
    def run(*args, code=0, binary=exe):
        p = subprocess.run([str(binary), "--data-dir", str(root), *args], capture_output=True, encoding="utf-8", input="", timeout=25)
        assert p.returncode == code, (args, p.returncode, p.stdout, p.stderr)
        return p
    assert "Sinan" in run("--version").stdout
    assert not root.exists()
    run("agent", "run", code=2)
    listing = json.loads(run("actions", "list", "--query", "固定").stdout)
    assert any(x["id"] == "tabs.pin" for x in listing["actions"])
    assert not root.exists(), "Action search must be local and side-effect free"
    run("actions", "run", "tabs.pin", code=2)
    try:
        start = json.loads(run("agent", "start", "--json").stdout)
        assert Path(start["executable"]).samefile(agent)
        assert start["mode"] == "observe"
        assert start["pid"] == json.loads(run("agent", "start", "--json").stdout)["pid"]
        assert start["shortcuts"]["hook_active"] is False
        # An authentic CLI copied to a different bundle cannot control this agent.
        other = Path(temp) / "other" / "sinan.exe"
        other.parent.mkdir()
        other.write_bytes(exe.read_bytes())
        run("agent", "status", "--json", code=4, binary=other)
        assert json.loads(run("agent", "status", "--json").stdout)["pid"] == start["pid"]
        unexpected = exe.with_name("sinan-untrusted-client.exe")
        assert not unexpected.exists()
        try:
            unexpected.write_bytes(exe.read_bytes())
            # This client sees the correct adjacent Agent; rejection must come
            # from the server checking the actual connecting process role.
            run("agent", "status", "--json", code=1, binary=unexpected)
            assert json.loads(run("agent", "status", "--json").stdout)["pid"] == start["pid"]
        finally:
            unexpected.unlink(missing_ok=True)
    finally:
        run("agent", "stop", "--json")
print("Sinan process roles, action discovery and bundle isolation passed")
