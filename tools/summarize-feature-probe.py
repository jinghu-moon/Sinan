"""Export path-free native acceptance evidence from a dedicated probe result."""
import hashlib
import json
from pathlib import Path
import sys

source, binaries, destination = map(Path, sys.argv[1:4])
raw = json.loads(source.read_text(encoding="utf-8-sig"))
assert raw["native_checks_passed"], "Native probe did not pass"
assert raw["style_restore"]["restoration_confirmed"]
assert any(sample.get("style_applied") for sample in raw["style_samples"])


def controls(phase):
    result = []
    for element in raw[phase].get("elements", []):
        if (element.get("name") not in {"TabContainer", "SelectedBackgroundPath"}
                and element.get("kind") != "Microsoft.UI.Xaml.Controls.TextBlock"):
            continue
        clean = {key: element[key] for key in (
            "kind", "name", "ForegroundColor", "BackgroundColor", "FillColor"
        ) if key in element}
        for prop in ("FontWeight", "FontFamily"):
            if prop in element:
                clean[prop] = element[prop]["value"]
        result.append(clean)
    return result


applied = controls("visual_readback")
restored = controls("restored_visual_readback")
expired = controls("visual_after_lease_expiry")
assert any(e.get("FillColor") == "#FFFFC060" for e in applied)
assert any(e.get("ForegroundColor") == "#FFF04444" and e.get("FontWeight") == "Bold" for e in applied)
assert not any(e.get("FillColor") == "#FFFFC060" or e.get("ForegroundColor") == "#FFF04444" for e in restored)
assert not any(e.get("FillColor") == "#FFFFC060" or e.get("ForegroundColor") == "#FFF04444" for e in expired)

evidence = {
    "date": "2026-11-16",
    "version": "0.7.1" if "rendered_icon_checks_passed" in raw else "0.7.0",
    "explorerframe_version": "10.0.26100.8457",
    "explorerframe_sha256": "4e28d2076608df5525ec1b7b2ed8dc0e866f43bf2ded6a5e4213ccd1c2284cbe",
    "isolation": "Dedicated /separate Explorer; only probe-owned acceptance folders and tabs",
    "tested_module_sha256": {
        name: hashlib.sha256((binaries / name).read_bytes()).hexdigest()
        for name in ("sinan-features.dll", "sinan-style.dll")
    },
    "native_checks_passed": raw["native_checks_passed"],
    "remove_result": raw["remove_result"],
    "host_close_result": raw["host_close_result"],
    "protected_after_unpin": raw["protected_after_unpin"],
    "navigation_result": raw["navigation_result"],
    "still_present": raw["still_present"],
    "same_path_guids_differ": raw["native_guid"] != raw["same_path_peer_guid"],
    "same_path_peer_protected": raw["same_path_peer"].get("protection_confirmed", False),
    "same_path_peer_close": raw["same_path_peer_close"],
    "redirected": raw["after_navigation"]["redirected"],
    "directory_identity_valid": raw["after_navigation"]["locked_identity_valid"],
    "native_icon_bitmap_confirmed": raw["apply"]["icon_applied"],
    "native_icon_restore_confirmed": raw["restore"]["icon_restored"],
    "behavior_restore_confirmed": raw["restore"]["behavior_restored"],
    "style_restore": raw["style_restore"],
    "style_samples": raw["style_samples"],
    "applied_controls": applied,
    "restored_controls": restored,
    "lease_expiry": {
        "renewal_paused_ms": 11200,
        "protection_confirmed": raw["after_lease_expiry"]["protection_confirmed"],
        "close_after_expiry": raw["close_after_restore"],
        "controls": expired,
    },
    "visual_tree_diagnostics": {
        phase: raw[phase].get("diagnostic_warning", raw[phase].get("error", ""))
        for phase in ("visual_readback", "restored_visual_readback", "visual_after_lease_expiry")
    },
    "acceptance": raw["apply"]["capabilities"]["acceptance"],
    "limitations": [
        "Computer Use native pipe unavailable; no screenshot or manual visual acceptance",
        "Native function probes do not accept mouse, keyboard, menu, window-close or notification routes",
        "Theme switching, hover, DPI, input methods, login and shutdown still need manual acceptance",
        "XAML initialization can be pending; retired visual handles can leave diagnostic warnings",
    ],
}
if "rendered_icon_checks_passed" in raw:
    assert raw["rendered_icon_checks_passed"] and raw["native_window_icon_unchanged"]
    def visual(phase):
        result=[]
        for entry in raw[phase].get("target_visual_tree", []):
            item={key:entry[key] for key in ("kind","depth","ForegroundColor","FillColor") if key in entry}
            for prop in ("IconSource","Source","Data"):
                if prop in entry:item[prop+"Type"]=entry[prop]["type"]
            if "ActualWidth" in entry:item["width"]=entry["ActualWidth"]["value"]
            result.append(item)
        return result
    for phase in ("system_visual_readback","restored_visual_readback","visual_after_lease_expiry"):
        root=raw[phase]["target_visual_tree"][0]
        assert root["IconSource"]["type"]=="Microsoft.UI.Xaml.Controls.ImageIconSource", phase
    evidence["rendered_icon_checks_passed"]=True
    evidence["native_window_icon_untouched"]=raw["apply"]["native_window_icon_untouched"]
    evidence["native_window_icon_unchanged"]=raw["native_window_icon_unchanged"]
    evidence["rendered_icons"]={
        "pin":visual("visual_readback"),"lock":visual("lock_visual_readback"),
        "system":visual("system_visual_readback"),"restored":visual("restored_visual_readback"),
        "expired":visual("visual_after_lease_expiry"),
    }
    evidence["limitations"]=[
        "User supplied a screenshot confirming a pin in the native tab; taskbar preview preservation was probed independently",
        "Mouse/menu input routes, all themes/DPI and notifications still require full manual acceptance",
        "Retired diagnostics handles can leave warnings; rendered icons are checked from the live target visual tree",
    ]
destination.parent.mkdir(parents=True, exist_ok=True)
destination.write_text(json.dumps(evidence, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
print("Wrote native evidence; actual applied, restored and expired values checked.")
