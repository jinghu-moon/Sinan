"""Verify generated project boundaries and imports of the actual release binaries."""
from pathlib import Path
import struct
import sys
import xml.etree.ElementTree as ET

source, build, exe = map(Path, sys.argv[1:])
ns = {"m": "http://schemas.microsoft.com/developer/msbuild/2003"}
projects = {p.stem: p for p in build.rglob("*.vcxproj")}

def closure(name):
    result, pending = set(), [name]
    while pending:
        current = pending.pop()
        if current in result:
            continue
        result.add(current)
        tree = ET.parse(projects[current])
        pending.extend(Path(e.attrib["Include"]).stem for e in tree.findall(".//m:ProjectReference", ns))
    return result

# Build ordering references on the executable include runtime artifacts. Inspect
# the libraries which actually supply its implementation to distinguish these.
assert "sinan_agent_runtime" not in closure("sinan_application")
assert not {"sinan_application", "sinan_tui", "component", "dom", "screen"} & closure("sinan_agent_runtime")
assert not {"sinan_application", "sinan_agent_runtime", "sinan_tui"} & closure("sinan_windows")
if "sinan_tui" in projects:
    assert "sinan_agent_runtime" not in closure("sinan_tui")
else:
    assert not {"component", "dom", "screen"} & projects.keys()
cli = (source / "src/cli/main.cpp").read_text(encoding="utf-8")
for forbidden in ("ConfigStore", "require_agent_result", "agent_request", '"agent/runtime.hpp"'):
    assert forbidden not in cli, forbidden

def imports(path):
    data = path.read_bytes()
    pe = struct.unpack_from("<I", data, 0x3c)[0]
    sections, optional_size = struct.unpack_from("<H12xH", data, pe + 6)
    optional = pe + 24
    assert struct.unpack_from("<H", data, optional)[0] == 0x20b  # PE32+
    table = optional + optional_size
    def offset(rva):
        for index in range(sections):
            entry = table + index * 40
            virtual_size, address, raw_size, raw = struct.unpack_from("<IIII", data, entry + 8)
            if address <= rva < address + max(virtual_size, raw_size):
                return raw + rva - address
        raise AssertionError((path, rva))
    rva = struct.unpack_from("<I", data, optional + 120)[0]
    if not rva:
        return set()
    cursor, names = offset(rva), set()
    while any(data[cursor:cursor + 20]):
        name = offset(struct.unpack_from("<I", data, cursor + 12)[0])
        names.add(data[name:data.index(0, name)].decode("ascii").lower())
        cursor += 20
    return names

for binary in exe.parent.iterdir():
    if binary.name.startswith("sinan") and binary.suffix.lower() in (".exe", ".dll"):
        linked = imports(binary)
        if binary.name == "sinan-symbols.exe":
            assert {"dbghelp.dll", "winhttp.dll"} <= linked
        else:
            assert not {"dbghelp.dll", "winhttp.dll"} & linked, (binary, linked)
print("Generated dependency graph, consumer boundary and real PE imports passed")
