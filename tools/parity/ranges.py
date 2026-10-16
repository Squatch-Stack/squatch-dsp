#!/usr/bin/env python3
"""List every parameter-contract difference between a browser node type and its squatch-dsp block.

python3 -I ranges.py RIG_DIR ROOT OUT_DIR   (RIG_DIR/web/js/graph/types.js and ROOT/contracts/*.params.json)
Writes OUT_DIR/ranges.md. A row is a parameter whose name, range, default, unit or taper differs
(or that exists on one side only)."""
import json
import subprocess
import sys
from pathlib import Path

rig, root, out = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
here = Path(__file__).parent
js = json.loads(subprocess.run(["node", str(here / "ranges.mjs"), str(rig)], capture_output=True, text=True, check=True).stdout)
PAIRS = {"gate": "gate", "compressor": "comp", "overdrive": "trail-od", "reverb": "hall", "limiter": "limiter", "nam": "nam_amp", "cab": "cabinet"}
# the browser's name for a parameter -> the contract's key, where they differ by name only
RENAME = {"limiter": {"gain": "drive"}}
lines = ["| pair | parameter | browser | squatch-dsp |", "|---|---|---|---|"]
for jt, ck in PAIRS.items():
    contract = json.loads((root / "contracts" / f"{ck}.params.json").read_text())
    cp = {p["key"]: p for p in contract["params"] if p.get("role", "param") == "param"}
    jp = {RENAME.get(jt, {}).get(k, k): (k, v) for k, v in js[jt].items()}
    for key in sorted(set(cp) | set(jp)):
        c, j = cp.get(key), jp.get(key)
        if c is None:
            lines.append(f"| {jt} | {key} | {j[1]['min']}..{j[1]['max']} {j[1].get('unit','')} (def {j[1]['def']}) | none |")
        elif j is None:
            lines.append(f"| {jt} | {key} | none | {c['min']}..{c['max']} {c.get('unit') or ''} (def {c['default']}) |")
        else:
            v = j[1]
            diffs = []
            if (v["min"], v["max"]) != (c["min"], c["max"]):
                diffs.append("range")
            if v["def"] != c["default"]:
                diffs.append("default")
            if (v.get("curve") == "log") != (c.get("taper") == "log"):
                diffs.append("taper")
            if diffs:
                lines.append(f"| {jt} | {key} ({', '.join(diffs)}) | {v['min']}..{v['max']} {v.get('unit','')} (def {v['def']}{', log' if v.get('curve')=='log' else ''}) | "
                             f"{c['min']}..{c['max']} {c.get('unit') or ''} (def {c['default']}{', log' if c.get('taper')=='log' else ''}) |")
(out / "ranges.md").write_text("\n".join(lines) + "\n")
print("\n".join(lines))
