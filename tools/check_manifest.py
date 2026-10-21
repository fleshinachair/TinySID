#!/usr/bin/env python3
"""Validates module.json against the Schwung module schema as used by the modules
installed on a Move: size budget, required fields, and that every param is defined once
and reachable on exactly one encoder page (max 8 knobs per page)."""
import json
import os
import sys

path = sys.argv[1] if len(sys.argv) > 1 else "module.json"
LIMIT = 8192

size = os.path.getsize(path)
with open(path, encoding="utf-8") as f:
    m = json.load(f)

errors = []
if size >= LIMIT:
    errors.append(f"{size} bytes is over the {LIMIT} byte limit")
for k in ("id", "name", "version", "api_version", "dsp", "capabilities"):
    if k not in m:
        errors.append(f"missing '{k}'")
if m.get("id") != "tinysid":
    errors.append("id must be 'tinysid'")
if m.get("api_version") != 2:
    errors.append("api_version must be 2")
caps = m.get("capabilities", {})
if "canvas_script" in caps and not os.path.exists(os.path.join(os.path.dirname(os.path.abspath(path)), caps["canvas_script"])):
    errors.append("capabilities.canvas_script does not exist next to module.json")
if caps.get("component_type") != "sound_generator":
    errors.append("component_type must be 'sound_generator'")

levels = caps.get("ui_hierarchy", {}).get("levels", {})
params, pages = {}, {}
for name, level in levels.items():
    for p in level.get("params", []):
        if "key" in p:
            if p["key"] in params:
                errors.append(f"param '{p['key']}' defined twice")
            params[p["key"]] = p
        elif "level" in p and p["level"] not in levels:
            errors.append(f"level link '{p['level']}' has no level")
    if "knobs" in level:
        pages[name] = level["knobs"]
if len(pages) < 2:
    errors.append(f"expected at least 2 encoder pages, found {len(pages)}")

# parameters a canvas page or feed uses are not knobs
feeds = {f["key"] for f in caps.get("feeds", [])}
page_canvases = {k for k, v in params.items() if v.get("type") == "canvas" and v.get("as_page")}

seen = []
for name, knobs in pages.items():
    if len(knobs) > 8:
        errors.append(f"page '{name}' has {len(knobs)} knobs (Move has 8 encoders)")
    seen += knobs
for k in seen:
    if k not in params:
        errors.append(f"knob '{k}' has no param definition")
for k in params:
    if k in page_canvases:
        if seen.count(k) != 0:
            errors.append(f"canvas page '{k}' must not also be a knob")
        continue
    if seen.count(k) != 1:
        errors.append(f"param '{k}' appears on {seen.count(k)} pages")

for k, p in params.items():
    t = p.get("type")
    if t in ("int", "float"):
        hi = p.get("max", p.get("max_param") and 256)
        if "min" not in p or hi is None:
            errors.append(f"'{k}' needs min and max")
        elif not (p["min"] <= p["default"] <= hi):
            errors.append(f"'{k}' default outside range")
    elif t == "enum":
        d = p["default"]
        if not (isinstance(d, int) and 0 <= d < len(p["options"])):
            errors.append(f"'{k}' default must be an in-range option index")
        if "short_options" in p and len(p["short_options"]) != len(p["options"]):
            errors.append(f"'{k}' short_options length differs from options")
    elif t == "canvas":
        script = p.get("canvas_script")
        if not script or not os.path.exists(os.path.join(os.path.dirname(os.path.abspath(path)), script)):
            errors.append(f"'{k}' canvas_script {script!r} does not exist next to module.json")
        keys = p.get("extra_keys", [])
        if len(keys) > 4:
            errors.append(f"'{k}' extra_keys has {len(keys)} entries (the host reads at most 4)")
        for ek in keys:
            if ek not in params and ek not in feeds:
                errors.append(f"'{k}' extra_key {ek!r} is not a parameter or a feed")
    elif t == "filepath":
        for need in ("root", "filter"):
            if need not in p:
                errors.append(f"'{k}' filepath needs '{need}'")
    else:
        errors.append(f"'{k}' has unknown type {t!r}")

print(f"module.json: {size} bytes (limit {LIMIT}), {len(params)} params, pages: " +
      ", ".join(f"{n}={len(k)} knobs" for n, k in pages.items()))
for e in errors:
    print("  ERROR:", e)
sys.exit(1 if errors else 0)
