#!/usr/bin/env python3
"""Usage: gen_ui_chain.py template.js canvas.js ui_hierarchy.json out.js

Builds the module's ui_chain.js: the template with the piano roll (canvas.js) inlined and the module's own hierarchy (the
document gen_ui_json.py derives from module.json) embedded, so the manifest stays the single source of truth. See
src/ui/ui_chain.template.js for why the module has one."""
import json
import sys

template, canvas, hierarchy, out = sys.argv[1:5]
with open(template, encoding="utf-8") as f:
    text = f.read()
with open(canvas, encoding="utf-8") as f:
    roll = f.read()
with open(hierarchy, encoding="utf-8") as f:
    doc = json.load(f)                      # must parse: a broken hierarchy would leave the module with no pages

for marker in ("/*@CANVAS@*/", "@HIERARCHY@"):
    if text.count(marker) != 1:
        sys.exit(f"{template}: marker {marker} must appear exactly once")

# the roll is a script ending in an IIFE; inlined as module code it must not be able to close the comment or the string it sits in
text = text.replace("/*@CANVAS@*/", roll)
text = text.replace("@HIERARCHY@", json.dumps(doc, separators=(",", ":")))
with open(out, "w", encoding="utf-8", newline="\n") as f:
    f.write(text)
print(f"ui_chain.js: {len(text.encode('utf-8'))} bytes")
