# SPDX-License-Identifier: MIT
"""Breakdowns, per-set verdicts, the pickup question, tracking over time and the data licences, for
the scorecard (standard library only).

score.py calls augment() once every platform is scored; report.py and report_html.py render
md_sections() and html_sections() (breakdowns_md.py). Everything lands in board["analysis"] (and
scorecard.json).

  per set       PASS/FAIL of every dataset row on its own, with the checks that failed
  breakdowns    the measures per string, fret range, pickup or guitar, instrument, style, note length
  pickup        does pickup position change what the tuner shows? Paired by string and fret across
                EGFxSet's five positions and IDMT's four setups: a Friedman test per measure, Kendall's
                W as the effect size, and each position's median paired difference from a reference
                position with a bootstrap 95% interval (stats.py)
  tracking      GuitarSet: error against the annotated contour by time since the note began, moving
                (bends, vibrato) against still, the display's lag behind a moving string, and how
                often short notes are read at all (tracking.py)
  worst         the worst notes of each new set, with file and time
  licences      the sets used, their licences and the attribution each requires
"""
import re

import metrics as M
from stats import friedman, paired, q
from tracking import dur_bin, tracking

NEW_SETS = ("guitarset", "nsynth", "idmt")
INTERNAL_ONLY = {"idmt"}
OPEN = {1: 64, 2: 59, 3: 55, 4: 50, 5: 45, 6: 40}  # EGFxSet string numbers: 1 = high E
FRETS = [(0, 0, "open"), (1, 4, "1-4"), (5, 9, "5-9"), (10, 14, "10-14"), (15, 24, "15-24")]
BANDS = ((28, 39, "E1-D#2 (bass)"), (40, 51, "E2-D#3"), (52, 63, "E3-D#4"), (64, 75, "E4-D#5"), (76, 88, "E5-E6"))

# The sets and what each requires. "public" says whether results or audio derived from the set may
# appear in anything shipped or published.
DATA_LICENCES = [
    {"set": "egfxset", "name": "EGFxSet (Clean subset)", "licence": "CC BY 4.0", "doi": "10.5281/zenodo.7044411",
     "attribution": "EGFxSet, Pedroza et al., ISMIR 2022 LBD. CC BY 4.0.", "public": "yes, with the attribution; say what was changed"},
    {"set": "guitar-techs", "name": "Guitar-TECHS (single notes)", "licence": "CC BY 4.0", "doi": "10.5281/zenodo.14963133",
     "attribution": "Guitar-TECHS, Pedroza et al., ICASSP 2025. CC BY 4.0.", "public": "yes, with the attribution; say what was changed"},
    {"set": "guitarset", "name": "GuitarSet v1.1.0 (hex debleeded, annotations)", "licence": "CC BY 4.0", "doi": "10.5281/zenodo.3371780",
     "attribution": "GuitarSet, Xi et al., ISMIR 2018. CC BY 4.0.", "public": "yes, with the attribution; say what was changed"},
    {"set": "nsynth", "name": "NSynth test split (guitar notes)", "licence": "CC BY 4.0", "doi": "none; magenta.tensorflow.org/datasets/nsynth",
     "attribution": "The NSynth Dataset, Engel et al., ICML 2017 (Google). CC BY 4.0. Resampled 16 to 48 kHz here.",
     "public": "yes, with the attribution; say it was resampled"},
    {"set": "idmt", "name": "IDMT-SMT-Guitar V2, dataset1", "licence": "CC BY-NC-ND 4.0", "doi": "10.5281/zenodo.7544110",
     "attribution": "IDMT-SMT-Guitar, Kehling et al., DAFx 2014 (Fraunhofer IDMT). CC BY-NC-ND 4.0.",
     "public": "NO. Internal testing only: never redistribute, never ship or publish derived audio, never commit it. "
               "These scores are internal and must be removed before this page is shared outside."},
    {"set": "owner", "name": "Our DI session di-20260924", "licence": "our own, unpublished", "doi": "-",
     "attribution": "-", "public": "no, unless we publish it"},
]


# ------------------------------------------------------------------------------------------ helpers --

def named_segments(case):
    """The case's segments that metrics.case_metrics scores, in its order."""
    return [s for s in case["segments"] if s["midi"] > 0 and s["truth"] != "disputed"]


def string_fret(value):
    m = re.match(r"s(\d)f(\d+)", value or "")
    return (int(m.group(1)), int(m.group(2))) if m else (None, None)


def fret_range(fret):
    return next((name for lo, hi, name in FRETS if lo <= fret <= hi), "other")


def gset_parts(track):
    """00_BN1-129-Eb_comp -> player 00, style BN, mode comp."""
    m = re.match(r"(\d\d)_([A-Za-z]+)\d+-\d+-[^_]+_(comp|solo)", track)
    return (m.group(1), m.group(2), m.group(3)) if m else ("?", "?", "?")


def nsynth_parts(value):
    m = re.match(r"p(\d+)v(\d+)(?:\|(.*))?", value)
    pitch, vel, quals = (int(m.group(1)), int(m.group(2)), m.group(3) or "") if m else (0, 0, "")
    return pitch, vel, [x for x in quals.split(",") if x]


def pitch_band(p):
    if p < 28 or p > 88:
        return "outside E1-E6"
    return next((name for lo, hi, name in BANDS if lo <= p <= hi), "?")


def _keys_fretted(case, _seg_case, _seg_res):
    s, f = string_fret(case["value"])
    return [("pickup" if case["dataset"] == "egfxset" else "setup", case["family"]), ("string", f"s{s}"), ("frets", fret_range(f))]


def _keys_guitarset(case, seg_case, seg_res):
    fam = case["family"]
    player, style, mode = gset_parts(case["value"])
    fret = seg_res["midi"] - OPEN[int(fam[1:])]
    return [("string", fam), ("frets", fret_range(max(0, fret))), ("style", style), ("player", player), ("mode", mode),
            ("note length", dur_bin(seg_case["end"] - seg_case["onset"]))]


def _keys_nsynth(case, _seg_case, _seg_res):
    fam = case["family"]
    pitch, vel, quals = nsynth_parts(case["value"])
    out = [("pitch", pitch_band(pitch))]
    if not 28 <= pitch <= 88:  # the other axes: notes in the tuner's range only
        return out
    out += [("source", fam.split("_")[1] if "_" in fam else fam), ("instrument", fam), ("velocity", str(vel))]
    out += [("quality", x) for x in quals or ["(none)"]]
    return out + [("subset", "in range, without tempo-synced" if "tempo-synced" not in quals else "in range, tempo-synced only")]


SEGMENT_KEYS = {"egfxset": _keys_fretted, "idmt": _keys_fretted, "guitarset": _keys_guitarset, "nsynth": _keys_nsynth,
                "guitar-techs": lambda case, _c, _r: [("player", case["family"])]}


def segment_keys(case, seg_case, seg_res):
    """(axis, key) pairs a segment is grouped under, for its dataset."""
    keys = SEGMENT_KEYS.get(case["dataset"])
    return keys(case, seg_case, seg_res) if keys else []


# ---------------------------------------------------------------------------------------- per set --

def per_set(board, platform):
    import report as R
    out = []
    for d, t in R.DATASET_ROWS:
        g = R.group(board, platform, "dataset", d, t)
        if not g:
            continue
        sk, ak = R.steady_checks(d), R.accuracy_checks(d)
        failed = [k for k in sk + ak if g["checks"].get(k) is False]
        out.append({"dataset": d, "tier": t, "cases": g["cases"], "segments": g["segments"],
                    "steadiness": all(g["checks"].get(k, True) for k in sk), "accuracy": all(g["checks"].get(k, True) for k in ak),
                    "failed": failed})
    return out


# ------------------------------------------------------------------------------------- breakdowns --

def breakdowns(results, cases, summarise):
    rows = {}
    for r in results:
        c = cases[r["id"]]
        if c["dataset"] not in NEW_SETS + ("egfxset", "guitar-techs"):
            continue
        for sc, sr in zip(named_segments(c), r.get("segments", [])):
            pseudo = {"frames": 0, "segments": [sr]}
            for axis, key in segment_keys(c, sc, sr):
                rows.setdefault((c["dataset"], axis, key), []).append(pseudo)
    return {"|".join(k): summarise(v) for k, v in sorted(rows.items())}


# ----------------------------------------------------------------------------------------- pickup --

MEASURES = [  # (key, label, from a segment's metrics, cap for None)
    ("wrong", "wrong-note frames", lambda s: s["wrong"] / s["live"] if s["live"] else None, None),
    ("first_correct", "first right note (s)", lambda s: s["first_correct"], 3.0),
    ("coverage", "coverage 0.1-2 s", lambda s: s["coverage"], None),
    ("settle_1c", "settle ±1 c (s)", lambda s: s["settle_1c"] if s["has_truth_hz"] else None, 3.0),
    ("note_err", "|note error| (c)", lambda s: abs(s["note_err"]) if s["note_err"] is not None else None, None),
    ("wobble", "wobble p95 (c)", lambda s: q(s["wobble"], 0.95), None),
]

PICKUP_SETS = {  # dataset: (grouping column, reference position, comparisons worth naming)
    "egfxset": ("pickup", "Neck"),
    "idmt": ("setup", "ibanez-power-strat-clean-neck-hu"),
}


def _positions(results, cases, d):
    """position -> note -> the note's segment metrics, for one pickup set."""
    per = {}
    for r in results:
        c = cases[r["id"]]
        if c["dataset"] == d and r.get("segments"):
            per.setdefault(c["family"], {})[c["value"]] = r["segments"][0]
    return per


def _capped(v, cap):
    return cap if v is None else min(v, cap)


def _values(per, notes, positions, measure):
    """position -> note -> the measure's value, capped where a missing value means "too slow"."""
    _key, _label, get, cap = measure
    vals = {p: {n: get(per[p][n]) for n in notes} for p in positions}
    if cap is None:
        return vals
    return {p: {n: _capped(v, cap) for n, v in vs.items()} for p, vs in vals.items()}


def _complete(vals, notes, positions):
    return [n for n in notes if all(vals[p][n] is not None for p in positions)]


def _mean(col):
    return sum(col) / len(col) if col else None


def _vs_ref(vals, complete, positions, ref):
    if ref not in positions:
        return {}
    return {p: paired([vals[p][n] - vals[ref][n] for n in complete]) for p in positions if p != ref}


def _measure(per, notes, positions, ref, measure):
    vals = _values(per, notes, positions, measure)
    complete = _complete(vals, notes, positions)
    chi2, p_value, w = friedman([[vals[p][n] for p in positions] for n in complete])
    col = {p: [vals[p][n] for n in complete] for p in positions}
    return {"label": measure[1], "complete": len(complete), "chi2": chi2, "p": p_value, "W": w,
            "median": {p: q(col[p], 0.5) for p in positions}, "mean": {p: _mean(col[p]) for p in positions},
            "vs_ref": _vs_ref(vals, complete, positions, ref)}


def _wrong_by_string(per, notes, p):
    share = MEASURES[0][2]
    return {s: q([share(per[p][n]) or 0.0 for n in notes if string_fret(n)[0] == s], 0.5) for s in range(1, 7)}


def _total(per, notes, positions, field):
    return {p: sum(per[p][n][field] for n in notes) for p in positions}


def _tallies(per, notes, positions):
    return {"wrong_by_string": {p: _wrong_by_string(per, notes, p) for p in positions},
            "octave_frames": _total(per, notes, positions, "octave"), "wrong_frames": _total(per, notes, positions, "wrong"),
            "live_frames": _total(per, notes, positions, "live")}


def pickup(results, cases):
    out = {}
    for d, (_, ref) in PICKUP_SETS.items():
        per = _positions(results, cases, d)
        if len(per) < 2:
            continue
        positions = sorted(per)
        notes = sorted(set.intersection(*(set(v) for v in per.values())))
        res = {"positions": positions, "reference": ref, "notes": len(notes),
               "measures": {m[0]: _measure(per, notes, positions, ref, m) for m in MEASURES}}
        # The same, per string: where does it differ, if it does?
        res.update(_tallies(per, notes, positions))
        out[d] = res
    return out


# ------------------------------------------------------------------------------------------ other --

def _nsynth_label_offsets(cases):
    """Cents from each NSynth note's MIDI label to its offline reference."""
    d = []
    for c in cases.values():
        if c["dataset"] != "nsynth":
            continue
        d += [M.cents(s["ref_hz"], 440.0 * 2 ** ((s["midi"] - 69) / 12)) for s in c["segments"] if s.get("ref_hz", 0) > 0 and s["midi"] > 0]
    return d


def _no_reference(c):
    return c["dataset"] == "nsynth" and not any(s.get("ref_hz", 0) > 0 for s in c["segments"])


def nsynth_tuning(cases):
    """How far NSynth's samples sit from their MIDI labels (offline reference against the label)."""
    a = [abs(x) for x in _nsynth_label_offsets(cases)]
    over = (lambda c: sum(1 for x in a if x > c) / len(a)) if a else (lambda c: None)
    return {"notes": len(a), "median_abs": q(a, 0.5), "p90_abs": q(a, 0.9), "over_10c": over(10), "over_25c": over(25),
            "no_reference": sum(1 for c in cases.values() if _no_reference(c))}


def worst_new(results, cases, worst, tier):
    """Each new set's worst notes (in the verdict's tiers: NSynth notes outside E1-E6 are left out).
    Wrong notes are ranked by how many frames were wrong, not by share: a short note is often
    wholly wrong on one or two frames."""
    out = {}
    for d in NEW_SETS:
        rs = [r for r in results if r["dataset"] == d and tier(cases[r["id"]]) != "outside"]
        w = worst(rs, n=8)
        wrong = [(s["wrong"], s["live"], r, s["wrong_at"]) for r in rs for s in r.get("segments", []) if s.get("wrong")]
        w["wrong note"] = [{"value": n, "share": n / max(1, live), "id": r["id"], "file": r["file"], "t": t}
                           for n, live, r, t in sorted(wrong, key=lambda x: -x[0])[:8]]
        out[d] = w
    return out


def _has_guitarset(frames, cases):
    return any(cases.get(k, {}).get("dataset") == "guitarset" for k in frames)


def _platform(a, p, v, cases, ctx):
    a["per_set"][p] = per_set(ctx["board"], p)
    if not any(r["dataset"] in NEW_SETS + ("egfxset",) for r in v["results"]):
        return
    a["breakdowns"][p] = breakdowns(v["results"], cases, ctx["summarise"])
    a["pickup"][p] = pickup(v["results"], cases)
    a["worst"][p] = worst_new(v["results"], cases, ctx["worst"], ctx["tier"])
    if p in ctx["frames"] and _has_guitarset(ctx["frames"][p], cases):
        a["tracking"][p] = tracking(ctx["frames"][p], ctx["truth"], cases)


def _traces(a, ctx):
    """The two worst-tracked GuitarSet notes, drawn from the reference platform's frames."""
    p0 = "mac-b16" if "mac-b16" in a["tracking"] else next(iter(a["tracking"]), None)
    if not p0:
        return
    frames = ctx["frames"][p0]
    for w in a["tracking"][p0]["worst"][:2]:
        if w["id"] in frames:
            a["traces"][w["id"]] = ctx["trace_of"](frames[w["id"]], ctx["truth"].get(w["id"], []),
                                                   f"GuitarSet, worst tracking: median {w['value']:.1f} c from the annotated contour over the note at "
                                                   f"{w['onset']:.2f} s; largest at {w['t']:.2f} s.", w["t"])


def augment(board, all_frames, all_truth, hooks):
    """Everything above, for every platform that scored any of it. hooks: score.py's summarise,
    worst, trace_of and tier."""
    cases = board["cases"]
    ctx = {"board": board, "frames": all_frames, "truth": all_truth, **hooks}
    a = {"per_set": {}, "breakdowns": {}, "pickup": {}, "tracking": {}, "worst": {}, "licences": DATA_LICENCES,
         "internal_only": sorted(INTERNAL_ONLY), "nsynth_tuning": nsynth_tuning(cases), "traces": {}}
    for p, v in board["platforms"].items():
        _platform(a, p, v, cases, ctx)
    _traces(a, ctx)
    board["analysis"] = a
