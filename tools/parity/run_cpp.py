#!/usr/bin/env python3
"""Run every case through the squatch-dsp block: python3 -I run_cpp.py PARITY_BLOCK RIG_DIR OUT_DIR."""
import json
import subprocess
import sys
from pathlib import Path

import numpy as np

exe, rig, out = sys.argv[1], Path(sys.argv[2]), Path(sys.argv[3])
(out / "cpp").mkdir(exist_ok=True)
cases = json.loads((out / "cases.json").read_text())
lat = {}
for c in cases:
    cpp = c["cpp"]
    args = [exe, cpp["type"], str(out / "in" / f"{c['input']}.f32"), str(out / "cpp" / f"{c['id']}.f32")]
    # the gain the harness applies before the block: done here so both sides get the same samples
    x = np.fromfile(out / "in" / f"{c['input']}.f32", dtype="<f4") * np.float32(10 ** (c["gainDb"] / 20))
    gained = out / "in" / f"_{c['id']}.f32"
    x.astype("<f4").tofile(gained)
    args[2] = str(gained)
    args += ["--tail", str(c["tail"])]
    if cpp["type"] == "nam":
        args += ["--nam", str(rig / "web/models" / cpp["model"]), "--nam-normalize", str(cpp.get("normalize", 1))]
    if cpp.get("ir"):
        args += ["--ir", str(out / "in" / f"ir_{c['id']}.f32")]
    args += [f"{k}={v}" for k, v in cpp["params"].items()]
    r = subprocess.run(args, capture_output=True, text=True)
    if r.returncode:
        sys.exit(f"{c['id']}: {r.stderr.strip()}")
    lat[c["id"]] = json.loads(r.stdout.strip().splitlines()[-1])["latency"]
    gained.unlink()
(out / "cpp_latency.json").write_text(json.dumps(lat))
print(f"{len(cases)} C++ renders")
