#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Compare two tuner frame CSVs (hosts/common/tuner_frames.hpp) row by row.

    tuner_csv_diff.py REFERENCE.csv OTHER.csv [--cents-tol 0.05] [--json]

Rows are matched by t_ms. Prints how many rows are byte-identical, whether every frame agrees on
valid / note / octave, the largest cents and Hz differences where both are valid, and how many
shown_cents differ. Exit 0 when the two give the same readings: the same frames, the same
validity and note on every frame, cents within --cents-tol and at most 0.5 % of shown_cents off
by 0.1 (a value sitting on a rounding edge). Standard library only.
"""
import argparse
import csv
import json
import math
import sys


def load(path):
    with open(path, newline="") as f:
        rows = [r for r in csv.reader(f) if r and not r[0].startswith("#")]
    if not rows or rows[0][0] != "t_ms":
        raise SystemExit(f"{path}: no t_ms header")
    return {r[0]: r for r in rows[1:]}


def num(s):
    return float(s) if s not in ("", "nan") else math.nan


def compare(ref, other):
    s = {"frames_ref": len(ref), "frames_other": len(other), "missing": 0, "identical": 0,
         "valid_mismatch": 0, "note_mismatch": 0, "both_valid": 0, "max_dcents": 0.0,
         "mean_dcents": 0.0, "max_dhz": 0.0, "shown_mismatch": 0, "shown_max_d": 0.0, "first_diff": None}
    total = 0.0
    for t, a in ref.items():
        b = other.get(t)
        if b is None:
            s["missing"] += 1
            continue
        if a == b:
            s["identical"] += 1
            continue
        s["first_diff"] = s["first_diff"] or {"ref": ",".join(a), "other": ",".join(b)}
        total += row_diff(a, b, s)
    s["mean_dcents"] = total / s["both_valid"] if s["both_valid"] else 0.0
    return s


def row_diff(a, b, s):
    """Account one differing row; returns its cents difference (0 unless both valid)."""
    if a[5] != b[5]:
        s["valid_mismatch"] += 1
    if a[1:3] != b[1:3]:
        s["note_mismatch"] += 1
    if a[6] != b[6]:
        s["shown_mismatch"] += 1
        d = abs(num(a[6]) - num(b[6]))
        s["shown_max_d"] = max(s["shown_max_d"], d if not math.isnan(d) else math.inf)
    if a[5] != "1" or b[5] != "1":
        return 0.0
    s["both_valid"] += 1
    dc = abs(num(a[3]) - num(b[3]))
    s["max_dcents"] = max(s["max_dcents"], dc)
    s["max_dhz"] = max(s["max_dhz"], abs(num(a[4]) - num(b[4])))
    return dc


def same(s, tol):
    frames = s["frames_ref"]
    return (s["missing"] == 0 and s["frames_other"] == frames and s["valid_mismatch"] == 0
            and s["note_mismatch"] == 0 and s["max_dcents"] <= tol
            and s["shown_mismatch"] <= 0.005 * frames and s["shown_max_d"] <= 0.1 + 1e-9)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("reference")
    ap.add_argument("other")
    ap.add_argument("--cents-tol", type=float, default=0.05)
    ap.add_argument("--json", action="store_true")
    a = ap.parse_args()
    s = compare(load(a.reference), load(a.other))
    s["same"] = same(s, a.cents_tol)
    if a.json:
        print(json.dumps(s))
    else:
        print(f"frames {s['frames_ref']}/{s['frames_other']}  identical {s['identical']}  "
              f"valid-mismatch {s['valid_mismatch']}  note-mismatch {s['note_mismatch']}  "
              f"max |dcents| {s['max_dcents']:.4f}  max |dHz| {s['max_dhz']:.5f}  "
              f"shown-mismatch {s['shown_mismatch']}  -> {'SAME' if s['same'] else 'DIFFERENT'}")
        if s["first_diff"]:
            print(f"  first difference:\n    ref   {s['first_diff']['ref']}\n    other {s['first_diff']['other']}")
    return 0 if s["same"] else 1


if __name__ == "__main__":
    sys.exit(main())
