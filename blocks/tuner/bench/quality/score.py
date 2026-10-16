#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Scorecard for the tuner quality suite (standard library only).

  score.py scorecard --run DIR [--run DIR ...] --out DIR    every platform's frames in each run
                    [--baseline DIR ...]                    ... with before -> after against earlier runs
  score.py file --case X.case.json --truth X.truth.csv --frames LOG.csv   one frame log (the phone)
  score.py diff A.csv B.csv                                 two frame logs of one file, line by line
  score.py manifest --pack DIR                              the phone pack's manifest.json
  score.py collect --pack DIR --logs DIR --platform NAME --out RUN   a device's per-file logs
                                                            (play-<id>.csv) as one scoreable run
  score.py compare-logs --a DIR --a-glob G --b DIR --b-glob G --name TEXT --out FILE
                                                            per-file logs of two platforms over the
                                                            same WAVs, line by line (scorecard --extra)

A run directory is what tuner_quality writes: cases.jsonl, truth-<dataset>.csv and
frames-<platform>-b<block>-<dataset>.csv. Thresholds are in THRESHOLDS; metrics.py defines
every measure.
"""
import argparse
import glob
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import metrics as M  # noqa: E402
import breakdowns  # noqa: E402
import platforms as PL  # noqa: E402
import report  # noqa: E402

# Pass thresholds. Steadiness is the complaint that started this suite: the number on screen must not wobble more
# than half a cent peak-to-peak over any 300 ms while the string holds its pitch.
THRESHOLDS = {
    "wobble_p95": 0.5,      # c, detrended peak-to-peak of the shown number over 300 ms (p95)
    "wobble_vs_truth_p95": 0.5,  # c, the same for (shown - truth): what the tuner adds (p95)
    "jumps_per_min": 1.0,   # steps > 3 c between frames on one note, per minute of live display
    "pp_steady_p95": 0.5,   # c, raw peak-to-peak over 300 ms while the truth is steady (p95)
    "delta_p90": 0.2,       # c, frame-to-frame change of the shown number (p90)
    "flips_per_s": 1.0,     # direction reversals of the shown number per second
    "wrong_rate": 0.01,     # share of live frames on the wrong note (after 100 ms)
    "err_p95": 1.0,         # c, |error| against the truth, steady state (p95)
    "err_median": 0.1,      # c, |error| median
    "note_err_median": 1.0,  # c, real notes: median |note error| against the offline reference
    "coverage": 0.9,        # share of 0.1-2 s with a live reading (median over notes)
    "settle_1c_median": 1.0,  # s to settle within +-1 c (median)
}
LOWER_IS_BETTER = {"coverage": False}

# Synthetic conditions outside what a tuner can be asked to read cleanly: reported, not headlined.
STRESS = {("noise", "SNR=10dB"), ("noise", "SNR=20dB"), ("level", "-60dBFS"), ("hum", "50Hz-20dB"), ("hum", "60Hz-20dB"),
          ("decay", "T60=0.6s"), ("stiffness", "B=2e-03"), ("glide", "40c")}
MOVING = {"vibrato", "bend"}


def q(v, p):
    if not v:
        return None
    s = sorted(v)
    i = p * (len(s) - 1)
    lo = int(i)
    return s[lo] if lo + 1 >= len(s) else s[lo] * (1 - (i - lo)) + s[lo + 1] * (i - lo)


def tier(c):
    if c["dataset"] == "regression":
        return "regression"  # built to fail the way real guitars did: reported, kept out of the verdict
    if c["dataset"].startswith("owner"):
        return "free" if c["family"] in ("free", "idle") else "core"
    if c["dataset"] == "guitarset":
        return "moving"  # real playing: bends, vibrato, slides and short notes, reported as the synthetic moving set is
    if c["dataset"] == "nsynth" and not 28 <= breakdowns.nsynth_parts(c["value"])[0] <= 88:
        return "outside"  # beyond the tuner's configured range (E1 to E6): reported, kept out of the verdict
    if c["dataset"] != "synthetic":
        return "core"
    if c["family"] in MOVING:
        return "moving"
    return "stress" if (c["family"], c["value"]) in STRESS else "core"


def pooled(cases):
    """Pool per-segment measures over cases."""
    p = {k: [] for k in ("err", "note_err", "first_correct", "settle_1c", "settle_01c", "coverage", "delta", "wobble", "pp_steady", "wobble_vs_truth")}
    p.update(live=0, wrong=0, octave=0, flips=0, seconds=0.0, segments=0, never_1c=0, never_01c=0, truth_hz=0, false_frames=0, frames=0,
             jumps=0, disputed=0)
    for c in cases:
        p["frames"] += c["frames"]
        p["false_frames"] += c.get("false_frames", 0)
        p["disputed"] += c.get("disputed", 0)
        for s in c.get("segments", []) + ([c["free"]] if "free" in c else []):
            add_segment(p, s)
    return p


def add_segment(p, s):
    for k in ("err", "delta", "wobble", "pp_steady", "wobble_vs_truth"):
        p[k] += s.get(k, [])
    p["flips"] += s.get("flip_count", 0)
    p["jumps"] += s.get("jumps", 0)
    p["seconds"] += s.get("seconds", 0.0)
    if "live" not in s:
        return
    p["segments"] += 1
    p["live"] += s["live"]
    p["wrong"] += s["wrong"]
    p["octave"] += s["octave"]
    for k in ("note_err", "first_correct", "coverage"):
        if s[k] is not None:
            p[k].append(abs(s[k]) if k == "note_err" else s[k])
    if s["has_truth_hz"]:
        p["truth_hz"] += 1
        for k, never in (("settle_1c", "never_1c"), ("settle_01c", "never_01c")):
            if s[k] is None:
                p[never] += 1
            else:
                p[k].append(s[k])


def summarise(cases):
    p = pooled(cases)
    err = [abs(e) for e in p["err"]]
    out = {"cases": len(cases), "segments": p["segments"], "frames": p["frames"],
           "wrong_rate": p["wrong"] / p["live"] if p["live"] else None, "octave_rate": p["octave"] / p["live"] if p["live"] else None,
           "err_median": q(err, 0.5), "err_p95": q(err, 0.95), "err_n": len(err),
           "note_err_median": q(p["note_err"], 0.5), "note_err_p95": q(p["note_err"], 0.95),
           "first_correct_median": q(p["first_correct"], 0.5), "first_correct_p90": q(p["first_correct"], 0.9),
           "settle_1c_median": q(p["settle_1c"], 0.5), "settle_1c_p90": q(p["settle_1c"], 0.9),
           "settle_01c_median": q(p["settle_01c"], 0.5), "never_1c": p["never_1c"], "never_01c": p["never_01c"], "truth_hz": p["truth_hz"],
           "coverage": q(p["coverage"], 0.5), "delta_median": q(p["delta"], 0.5), "delta_p90": q(p["delta"], 0.9),
           "flips_per_s": p["flips"] / p["seconds"] if p["seconds"] else None,
           "wobble_p95": q(p["wobble"], 0.95), "wobble_max": max(p["wobble"]) if p["wobble"] else None, "wobble_n": len(p["wobble"]),
           "pp_steady_p95": q(p["pp_steady"], 0.95), "pp_steady_n": len(p["pp_steady"]),
           "wobble_vs_truth_p95": q(p["wobble_vs_truth"], 0.95), "wobble_vs_truth_n": len(p["wobble_vs_truth"]),
           "jumps": p["jumps"], "jumps_per_min": 60.0 * p["jumps"] / p["seconds"] if p["seconds"] else None,
           "false_frames": p["false_frames"], "disputed": p["disputed"]}
    out["checks"] = checks(out)
    return out


def checks(s):
    out = {}
    for k, limit in THRESHOLDS.items():
        v = s.get(k)
        if v is None:
            continue
        out[k] = v >= limit if LOWER_IS_BETTER.get(k) is False else v <= limit
    return out


def load_run(run):
    """A run's cases, truth and frame files; a frames-only run (another platform) has frames only."""
    cases = {}
    path = os.path.join(run, "cases.jsonl")
    if os.path.exists(path):
        with open(path) as f:
            for line in f:
                c = json.loads(line)
                cases[c["id"]] = c
    truth = {}
    for path in glob.glob(os.path.join(run, "truth-*.csv")):
        truth.update(M.load_truth(path))
    platforms = {}
    for path in glob.glob(os.path.join(run, "frames-*.csv")):
        m = re.match(r"frames-(.+)-b(\d+)-(.+)\.csv", os.path.basename(path))
        platforms.setdefault(f"{m.group(1)}-b{m.group(2)}", []).append(path)
    return cases, truth, platforms


def score_platform(cases, truth, paths):
    out = []
    for path in paths:
        for cid, frames in M.load_frames(path).items():
            if cid in cases:
                out.append(M.case_metrics(cases[cid], frames, truth.get(cid, [])))
    return out


def groups(results, cases):
    """Summary rows: per dataset (and tier), per synthetic family, per synthetic condition."""
    rows = {}
    for r in results:
        c = cases[r["id"]]
        t = tier(c)
        rows.setdefault(("dataset", r["dataset"], t), []).append(r)
        rows.setdefault(("family", r["dataset"], r["family"]), []).append(r)
        if r["dataset"] == "synthetic":
            rows.setdefault(("condition", r["family"], r["value"]), []).append(r)
        if r["dataset"].startswith("owner"):
            rows.setdefault(("take", r["dataset"] + "/" + r["value"]), []).append(r)
    return {k: summarise(v) for k, v in rows.items()}


def segment_worst(r, s):
    """Candidates for the worst-case lists from one segment of one case."""
    out = []
    if s.get("wobble"):
        i = max(range(len(s["wobble"])), key=lambda j: s["wobble"][j])
        out.append(("wobble", s["wobble"][i], s["wobble_at"][i]))
    if s.get("wrong"):
        out.append(("wrong note", s["wrong"] / max(1, s["live"]), s["wrong_at"]))
    if s.get("note_err") is not None:
        out.append(("note error", abs(s["note_err"]), s["onset"]))
    return [(k, v, r, t) for k, v, t in out]


def worst(results, n=12):
    """The worst cases for wobble, wrong notes and note error, each with file and time."""
    flat = []
    for r in results:
        for s in r.get("segments", []) + ([r["free"]] if "free" in r else []):
            flat += segment_worst(r, s)
        if r.get("false_frames"):
            flat.append(("false reading", r["false_frames"], r, r["false_at"]))
    out = {}
    for kind in ("wobble", "wrong note", "note error", "false reading"):
        items = sorted((f for f in flat if f[0] == kind), key=lambda f: -f[1])[:n]
        out[kind] = [{"value": v, "id": r["id"], "file": r["file"], "t": t} for _, v, r, t in items]
    return out


def truth_on_screen(f, t):
    """The truth in cents against the note on screen, or None when unknown or 50 c or more away."""
    note = f["midi"] or (t["midi"] if t else 0)
    if t is None or t["hz"] <= 0 or note <= 0:
        return None
    c = M.cents(t["hz"], 440.0 * 2 ** ((note - 69) / 12.0))
    return round(c, 3) if abs(c) <= 50.0 else None


def trace_of(frames, truth, why, at=None):
    """Shown number and truth over time, for the page: the whole take, or 3 s either side of `at`
    in a long one. Truth far from the note on screen (another note shown) is left out rather
    than drawn off the scale."""
    pairs = M.join(frames, truth)
    if at is not None and frames and frames[-1]["t"] > 20.0:
        pairs = [(f, t) for f, t in pairs if abs(f["t"] - at) <= 3.0]
    return {"why": why, "t": [round(f["t"], 3) for f, _ in pairs], "shown": [f["shown"] if f["valid"] else None for f, _ in pairs],
            "truth": [truth_on_screen(f, t) for f, t in pairs]}


def pick_traces(board, frames, truth):
    """Our open E take (the complaint), and the worst wobble in the real and synthetic core sets."""
    p = "mac-b16" if "mac-b16" in board["platforms"] else sorted(board["platforms"])[0]
    chosen = {"owner-03-open-E2": ("Our open low E take through the Mac display path: the reading the phone was unusable on.", None)}
    for w in board["platforms"][p]["worst"]["wobble"]:
        c = board["cases"][w["id"]]
        if tier(c) != "core" or c["dataset"] in [board["cases"][k]["dataset"] for k in chosen]:
            continue
        chosen[w["id"]] = (f"Worst wobble in {c['dataset']}: {w['value']:.2f} c peak-to-peak over the 300 ms ending at {w['t']:.2f} s.", w["t"])
        if len(chosen) >= 4:
            break
    return {cid: trace_of(frames[p][cid], truth.get(cid, []), why, at) for cid, (why, at) in chosen.items() if cid in frames[p]}


def score_runs(board, runs):
    """Every platform in every run, against the union of the runs' cases and truth."""
    all_frames, all_truth = {}, {}
    for cases, truth, _ in runs:
        board["cases"].update(cases)
        all_truth.update(truth)
    for _, _, platforms in runs:
        for name, paths in sorted(platforms.items()):
            results = score_platform(board["cases"], all_truth, paths)
            groups_ = {"|".join(k): v for k, v in groups(results, board["cases"]).items()}
            # The new sets have their own worst lists (breakdowns.py): these stay the original sets'.
            original = [r for r in results if r["dataset"] not in breakdowns.NEW_SETS]
            board["platforms"][name] = {"worst": worst(original), "results": results, "groups": groups_}
            all_frames[name] = PL.platform_frames(paths)
            print(f"{name}: {len(results)} cases scored", file=sys.stderr)
    return all_frames, all_truth


def read_json(paths):
    out = []
    for path in paths or []:
        with open(path) as f:
            out.append(json.load(f))
    return out


def read_notes(paths):
    notes = []
    for path in paths or []:
        with open(path) as f:
            notes += [p.strip() for p in f.read().split("\n\n") if p.strip()]
    return notes


def baseline_board(runs):
    """The earlier runs scored the same way, for the before -> after tables."""
    before = {"thresholds": THRESHOLDS, "platforms": {}, "comparisons": [], "cases": {}}
    score_runs(before, [load_run(run) for run in runs])
    return before


def scorecard(args):
    board = {"thresholds": THRESHOLDS, "platforms": {}, "comparisons": [], "cases": {}}
    if args.baseline:
        board["baseline"] = baseline_board(args.baseline)
    all_frames, all_truth = score_runs(board, [load_run(run) for run in args.run])
    board["comparisons"] = PL.all_pairs(all_frames, board["cases"]) + read_json(args.extra)
    board["traces"] = pick_traces(board, all_frames, all_truth)
    breakdowns.augment(board, all_frames, all_truth, {"summarise": summarise, "worst": worst, "trace_of": trace_of, "tier": tier})
    os.makedirs(args.out, exist_ok=True)
    report.write_all(board, args.out, read_notes(args.note))


def score_file(args):
    with open(args.case) as f:
        case = json.load(f)
    frames = M.load_frames(args.frames, case_col=False)[""]
    truth = M.load_truth(args.truth, case_col=False)[""]
    s = summarise([M.case_metrics(case, frames, truth)])
    print(json.dumps(s, indent=1))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("scorecard")
    s.add_argument("--run", action="append", required=True)
    s.add_argument("--out", required=True)
    s.add_argument("--note", action="append", help="a text file of paragraphs for the top of the scorecard")
    s.add_argument("--extra", action="append", help="a comparison from compare-logs")
    s.add_argument("--baseline", action="append", help="an earlier run, for before -> after columns")
    f = sub.add_parser("file")
    f.add_argument("--case", required=True)
    f.add_argument("--truth", required=True)
    f.add_argument("--frames", required=True)
    d = sub.add_parser("diff")
    d.add_argument("a")
    d.add_argument("b")
    m = sub.add_parser("manifest")
    m.add_argument("--pack", required=True)
    x = sub.add_parser("compare-logs")
    for k in ("--a", "--a-glob", "--b", "--b-glob", "--name", "--out"):
        x.add_argument(k, required=True)
    x.add_argument("--b-prefix", default="")
    c = sub.add_parser("collect")
    for k in ("--pack", "--logs", "--platform", "--out"):
        c.add_argument(k, required=True)
    args = ap.parse_args()
    commands = {"scorecard": scorecard, "file": score_file, "diff": PL.diff_files, "collect": PL.collect,
                "compare-logs": PL.compare_logs, "manifest": lambda a: report.write_manifest(a.pack)}
    return commands[args.cmd](args)


if __name__ == "__main__":
    sys.exit(main() or 0)
