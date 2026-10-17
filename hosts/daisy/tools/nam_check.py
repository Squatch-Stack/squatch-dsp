#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Check the board's A2-Lite output against the Mac.

    hosts/daisy/tools/nam_check.py [--model PATH.nam] [--keep DIR] [LOG.txt]

Reads the "DUMP" lines the seed3-nam-parity image sends (from LOG.txt, or live from the Seed3's serial port), rebuilds the
4,800 input and output samples from their float bits, and compares the board's output with
  1. the same kernel built for this Mac (tools/nam_host.cpp): bit for bit, and
  2. NAM's float64 reference (tools/a2_reference.py, our numpy re-implementation of the network), fed the board's exact input bits.
Prints the number of identical samples, the largest difference, and the error-to-signal ratio against each.
numpy is needed (the reference's), standard library otherwise.
"""
import argparse
import math
import re
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[3]
REF = Path(__file__).resolve().parent / "a2_reference.py"  # the float64 reference, next to this file
DEFAULT_MODEL = ROOT / "build/nam-model/A2.nam"  # fetched by tools/fetch-nam-model


def read_dump(lines):
    pairs = {}
    for ln in lines:
        m = re.match(r"D (\d+)((?: [0-9a-f]{8}:[0-9a-f]{8})+)\s*$", ln.strip())
        if m:
            for k, tok in enumerate(m.group(2).split()):
                a, b = tok.split(":")
                pairs[int(m.group(1)) + k] = (int(a, 16), int(b, 16))
    n = max(pairs) + 1 if pairs else 0
    if len(pairs) != n or n == 0:
        sys.exit(f"nam_check: incomplete dump ({len(pairs)} of {n} samples)")
    bits = np.array([pairs[i] for i in range(n)], dtype=np.uint32)
    return bits[:, 0].view(np.float32), bits[:, 1].view(np.float32)


def live_lines():
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import flash
    port = flash.wait_for(lambda: flash.find_port(flash.seed3_serial()), 20)
    if not port:
        sys.exit("nam_check: no Seed3 serial port")
    return flash.read_lines(port, 22.0)


def esr_db(y, ref):
    e = y.astype(np.float64) - ref
    return 10 * math.log10(float(np.sum(e * e)) / float(np.sum(ref * ref)))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("log", nargs="?")
    ap.add_argument("--model", default=str(DEFAULT_MODEL))
    ap.add_argument("--keep")
    a = ap.parse_args()
    lines = Path(a.log).read_text().splitlines() if a.log else live_lines()
    x, y = read_dump(lines)
    with tempfile.TemporaryDirectory() as tmp:
        d = Path(a.keep or tmp)
        d.mkdir(parents=True, exist_ok=True)
        x.tofile(d / "in.f32")
        host = d / "nam_host"
        subprocess.run(["clang++", "-std=c++17", "-O2", f"-I{ROOT / 'include'}", str(ROOT / "hosts/daisy/tools/nam_host.cpp"),
                        "-o", str(host)], check=True)
        subprocess.run([str(host), str(d / "in.f32"), str(d / "host.f32")], check=True)
        h = np.fromfile(d / "host.f32", dtype=np.float32)
        x.astype("<f8").tofile(d / "in.f64")
        subprocess.run([sys.executable, str(REF), a.model, "--slim", "0.0", "--input", str(d / "in.f64"), "--out", str(d / "ref.f64")],
                       check=True, capture_output=True)
        r = np.fromfile(d / "ref.f64", dtype="<f8")
    same = int(np.sum(y.view(np.uint32) == h.view(np.uint32)))
    print(f"nam_check: {len(y)} samples; output peak {float(np.max(np.abs(y))):.4f}, rms {float(np.sqrt(np.mean(y.astype(np.float64) ** 2))):.4f}")
    print(f"  board vs the same kernel on the Mac: {same} of {len(y)} bit-identical, largest difference {float(np.max(np.abs(y - h))):.3e}")
    print(f"  board vs NAM's float64 reference: largest error {float(np.max(np.abs(y - r))):.3e}, ESR {esr_db(y, r):.1f} dB")
    print(f"  Mac kernel vs NAM's float64 reference: largest error {float(np.max(np.abs(h - r))):.3e}, ESR {esr_db(h, r):.1f} dB")
    return 0


if __name__ == "__main__":
    sys.exit(main())
