#!/usr/bin/env python3
"""How close can the C++ Trail OD get to the browser's, by knobs alone?  python3 -I fit_od.py PARITY_BLOCK OUT_DIR

For three browser settings (cases odfit-*), the C++ block is run over a grid of Drive and Tone (Level
fixed, the best gain applied afterwards) and the pair with the lowest ESR after gain is kept.
That residual is the floor mapping alone can reach: what is left is the algorithm.
Writes OUT_DIR/od_fit.json and appends a table to OUT_DIR/results.md."""
import json
import subprocess
import sys
from pathlib import Path

import numpy as np

exe, out = sys.argv[1], Path(sys.argv[2])
cases = {c["id"]: c for c in json.loads((out / "cases.json").read_text()) if c["pair"] == "odfit"}
grid = [round(v, 2) for v in np.arange(0.0, 1.0001, 0.1)]
res = {}
for cid, c in cases.items():
    j = np.fromfile(out / "js" / f"{cid}.f32", dtype="<f4").astype(np.float64)
    best = (9e9, None, None)
    base = None
    for d in grid:
        for t in grid:
            tmp = out / "cpp" / "_fit.f32"
            r = subprocess.run([exe, "cc.squatch.trail-od", str(out / "in" / f"{c['input']}.f32"), str(tmp), f"drive={d}", f"tone={t}", "level=-6"],
                               capture_output=True, text=True, check=True)
            k = np.fromfile(tmp, dtype="<f4").astype(np.float64)
            # the browser is 32 samples late: align
            n = min(len(j), len(k))
            a, b = j[32:n], k[: n - 32]
            g = np.dot(a, b) / np.dot(b, b)
            e = float(np.sum((g * b - a) ** 2) / np.sum(a * a))
            if (d, t) == (c["cpp"]["params"]["drive"], c["cpp"]["params"]["tone"]):
                base = e
            if e < best[0]:
                best = (e, d, t)
    res[cid] = dict(js=c["js"]["params"], same_knobs_esr_after_gain=base, best_esr_after_gain=best[0], best_drive=best[1], best_tone=best[2])
(out / "od_fit.json").write_text(json.dumps(res, indent=1))
lines = ["### od knob fit (grid 0.1 steps, riff first 6 s)", "", "| browser drive / tone | ESR after gain, same knobs | best C++ drive / tone | best ESR after gain |", "|---|---|---|---|"]
for cid, r in res.items():
    lines.append(f"| {r['js']['drive']} / {r['js']['tone']} | {r['same_knobs_esr_after_gain']:.4f} | {r['best_drive']} / {r['best_tone']} | {r['best_esr_after_gain']:.4f} |")
with open(out / "results.md", "a") as f:
    f.write("\n" + "\n".join(lines) + "\n")
print("\n".join(lines))
