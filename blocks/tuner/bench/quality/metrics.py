# SPDX-License-Identifier: MIT
"""Per-case metrics of a tuner display, from a frame log and its truth (standard library only).

A frame log is the shared format (io.hpp): t_ms,note,octave,cents,hz,valid,shown_cents. Any
platform that writes it (the Mac run, the Pi, the phone, the S3 probe) is scored by this code.

Definitions (all per note segment, times in seconds from the segment's onset):
  wrong_note     live frames after 0.1 s whose note is not the truth's note; octave = off by 12k
  err            1200 log2(hz / truth_hz) on live, correct-note frames 0.5 s or more after the
                 onset while the truth is steady (it moved < 0.2 c in the last 300 ms)
  note_err       median hz over 0.5-1.7 s against the note reference (real notes)
  first_valid    first live frame; first_correct: first live frame on the right note
  settle_1c, settle_01c   first time after which every live correct frame (to 3 s) is within
                 +-1 c / +-0.1 c of the truth at that moment; None if never
  coverage       share of frames live over 0.1-2.0 s (or to the segment's end)
  delta          |shown change| between consecutive live correct frames from 0.5 s
  flips          sign changes of the shown number's steps of 0.2 c or more, per second (a 0.1 c
                 step is the last digit dithering as a real string drifts across a rounding edge)
  wobble         peak-to-peak of the shown number about its own straight line, over every
                 300 ms (10 consecutive frames at 30 Hz) from 0.5 s: a number that flips high
                 and low scores high, a string drifting steadily does not
  pp_steady      raw peak-to-peak of the shown number over 300 ms windows where the truth is
                 steady throughout (the complaint that started this suite, taken literally)
  wobble_vs_truth   the wobble of (shown - truth) over the same windows, where the truth is known
                 on every frame: what the tuner adds to a string that really wavers
  jumps          steps of more than 3 c between consecutive live frames on the same note (a
                 reading that leaps and comes back is two), counted per minute of live display
"""
import csv
import math
from statistics import median

WINDOW = 10          # frames in 300 ms at 30 Hz
JUMP = 3.0           # c: a step this large between frames is a visible leap
FLIP_STEP = 0.15     # c: steps of one display digit (0.1) are dither on a drifting string, not flips
SUSTAIN_FROM = 0.5   # s after onset: the display has had time to settle
SCORE_FROM = 0.1     # s after onset: wrong-note counting starts


def fnum(s):
    return float(s) if s not in ("", None) else None


def load_frames(path, case_col=True):
    """{case: [frame dict]} from a bulk frame file (or {'': [...]} for a single log)."""
    out = {}
    with open(path, newline="") as f:
        for row in csv.DictReader(f):
            key = row.get("case", "") if case_col else ""
            out.setdefault(key, []).append(parse_frame(row))
    return out


def parse_frame(row):
    """One row. Tolerates the phone's play-*.csv variant: 0 for cents/hz and octave when not
    reading, and the page's number with three decimals (rounded here to what the screen shows)."""
    note, octave, valid = row["note"], row["octave"], row["valid"] == "1"
    midi = note_midi(note, int(octave)) if note else 0
    shown = fnum(row["shown_cents"])
    return {"t": float(row["t_ms"]) / 1000.0, "midi": midi, "cents": fnum(row["cents"]) if valid else None,
            "hz": fnum(row["hz"]) if valid else None, "valid": valid, "shown": round_tenth(shown) if shown is not None else None}


def round_tenth(c):
    """The page's formatCents to a number: one decimal, Math.round (halves up), -0.0 as 0.0."""
    v = math.floor(c * 10 + 0.5) / 10
    return 0.0 if v == 0 else v


NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]


def note_midi(name, octave):
    return NAMES.index(name) + 12 * (octave + 1)


def load_truth(path, case_col=True):
    out = {}
    with open(path, newline="") as f:
        for row in csv.DictReader(f):
            key = row.get("case", "") if case_col else ""
            out.setdefault(key, []).append({"t": float(row["t_ms"]) / 1000.0, "hz": float(row["truth_hz"]),
                                            "midi": int(row["truth_midi"]), "steady": row["steady"] == "1",
                                            "seg": int(row["segment"])})
    return out


def join(frames, truth):
    """Truth for each frame, by nearest time (logs from other platforms need not share the grid)."""
    if not truth:
        return [(f, None) for f in frames]
    out, j = [], 0
    for f in frames:
        while j + 1 < len(truth) and abs(truth[j + 1]["t"] - f["t"]) <= abs(truth[j]["t"] - f["t"]):
            j += 1
        out.append((f, truth[j] if abs(truth[j]["t"] - f["t"]) < 0.02 else None))
    return out


def cents(a, b):
    return 1200.0 * math.log2(a / b)


def line_residual_pp(ys):
    n = len(ys)
    xs = range(n)
    mx, my = (n - 1) / 2.0, sum(ys) / n
    sxx = sum((x - mx) ** 2 for x in xs)
    b = sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / sxx
    r = [y - (my + b * (x - mx)) for x, y in zip(xs, ys)]
    return max(r) - min(r)


def runs(pairs, keep):
    """Maximal runs of consecutive frames satisfying keep(frame, truth)."""
    out, cur = [], []
    for p in pairs:
        if keep(*p):
            cur.append(p)
        elif cur:
            out.append(cur)
            cur = []
    if cur:
        out.append(cur)
    return out


def steadiness(sustain_runs):
    """delta, flips, jumps, wobble and pp_steady over runs of live, correct-note sustain frames."""
    m = {"delta": [], "flip_count": 0, "seconds": 0.0, "wobble": [], "wobble_at": [], "pp_steady": [], "wobble_vs_truth": [], "jumps": 0}
    for run in sustain_runs:
        step_stats(run, m)
        for i in range(len(run) - WINDOW + 1):
            window_stats(run[i:i + WINDOW], m)
    return m


def step_stats(run, m):
    shown = [f["shown"] for f, _ in run]
    steps = [b - a for a, b in zip(shown, shown[1:])]
    m["delta"] += [abs(s) for s in steps]
    m["jumps"] += sum(1 for s in steps if abs(s) > JUMP)
    signs = [s > 0 for s in steps if abs(s) >= FLIP_STEP]
    m["flip_count"] += sum(1 for a, b in zip(signs, signs[1:]) if a != b)
    m["seconds"] += run[-1][0]["t"] - run[0][0]["t"]


def window_stats(w, m):
    ys = [f["shown"] for f, _ in w]
    m["wobble"].append(line_residual_pp(ys))
    m["wobble_at"].append(w[-1][0]["t"])
    if all(t is not None and t["steady"] for _, t in w):
        m["pp_steady"].append(max(ys) - min(ys))
    excess = [f["shown"] - truth_cents(f, t) for f, t in w if t is not None and t["hz"] > 0]
    if len(excess) == WINDOW:
        m["wobble_vs_truth"].append(line_residual_pp(excess))


def truth_cents(f, t):
    """The truth in cents against the note on screen."""
    return cents(t["hz"], 440.0 * 2 ** ((f["midi"] - 69) / 12.0))


def settle_time(scored, onset, tol):
    """First t after which every live correct frame up to 3 s is within tol of the truth."""
    last_bad, seen = None, False
    for f, t in scored:
        if f["t"] - onset > 3.0:
            break
        seen = True
        if abs(cents(f["hz"], t["hz"])) > tol:
            last_bad = f["t"]
    if not seen:
        return None
    after = [f["t"] for f, _ in scored if last_bad is None or f["t"] > last_bad]
    return (after[0] - onset) if after and after[0] - onset <= 3.0 else None


def in_segment(pairs, seg, a, b):
    return [(f, t) for f, t in pairs if seg["onset"] + a <= f["t"] < min(seg["end"], seg["onset"] + b)]


def live_correct(f, t):
    return f["valid"] and t is not None and t["midi"] > 0 and f["midi"] == t["midi"] and f["hz"]


def note_error(pairs, seg):
    """Median live hz over 0.5-1.7 s against the note reference, in cents."""
    if seg.get("ref_hz", 0) <= 0:
        return None
    hz = [f["hz"] for f, t in in_segment(pairs, seg, SUSTAIN_FROM, 1.7) if live_correct(f, t)]
    return cents(median(hz), seg["ref_hz"]) if len(hz) >= 5 else None


def labelled(t):
    return t is not None and t["midi"] > 0


def first_time(pairs, seg, keep):
    """Seconds from the onset to the first frame satisfying keep(frame, truth), or None."""
    for f, t in pairs:
        if keep(f, t):
            return f["t"] - seg["onset"]
    return None


def identity(pairs, seg):
    """Wrong-note and octave counts over live frames after 0.1 s, and the first valid times."""
    live = [(f, t) for f, t in in_segment(pairs, seg, SCORE_FROM, 1e9) if f["valid"] and labelled(t)]
    wrong = [(f, t) for f, t in live if f["midi"] != t["midi"]]
    whole = in_segment(pairs, seg, 0, 1e9)
    return {"live": len(live), "wrong": len(wrong), "octave": sum(1 for f, t in wrong if (f["midi"] - t["midi"]) % 12 == 0),
            "first_valid": first_time(whole, seg, lambda f, t: f["valid"]), "first_correct": first_time(whole, seg, live_correct),
            "wrong_at": wrong[-1][0]["t"] if wrong else None}


def accuracy(pairs, seg):
    """Steady-state errors against the truth, settle times and coverage."""
    scored = [(f, t) for f, t in in_segment(pairs, seg, 0, 1e9) if live_correct(f, t) and t["hz"] > 0]
    steady = [cents(f["hz"], t["hz"]) for f, t in scored if t["steady"] and f["t"] - seg["onset"] >= SUSTAIN_FROM]
    cov = in_segment(pairs, seg, SCORE_FROM, 2.0)
    return {"err": steady, "settle_1c": settle_time(scored, seg["onset"], 1.0), "settle_01c": settle_time(scored, seg["onset"], 0.1),
            "has_truth_hz": bool(scored), "coverage": sum(1 for f, _ in cov if f["valid"]) / len(cov) if cov else None,
            "note_err": note_error(pairs, seg)}


def segment_metrics(pairs, seg):
    m = {"onset": seg["onset"], "midi": seg["midi"], "truth": seg["truth"]}
    m.update(identity(pairs, seg))
    m.update(accuracy(pairs, seg))
    sustain = runs(in_segment(pairs, seg, SUSTAIN_FROM, 1e9), lambda f, t: live_correct(f, t) and f["shown"] is not None)
    m.update(steadiness(sustain))
    return m


def note_runs(pairs):
    """Maximal runs of live frames showing a number, on one note."""
    out, cur = [], []
    for f, t in pairs:
        shows = f["valid"] and f["shown"] is not None
        if shows and cur and f["midi"] == cur[-1][0]["midi"]:
            cur.append((f, t))
            continue
        if cur:
            out.append(cur)
        cur = [(f, t)] if shows else []
    return out + ([cur] if cur else [])


def free_metrics(pairs):
    """No truth: steadiness over runs of one displayed note lasting 0.8 s or more, from 0.5 s in."""
    long_runs = [r for r in note_runs(pairs) if r[-1][0]["t"] - r[0][0]["t"] >= 0.8]
    keep = [[p for p in r if p[0]["t"] - r[0][0]["t"] >= SUSTAIN_FROM] for r in long_runs]
    return steadiness([r for r in keep if r])


def idle_metrics(frames, out):
    out["false_frames"] = sum(1 for f in frames if f["valid"])
    out["false_at"] = next((f["t"] for f in frames if f["valid"]), None)
    return out


def case_metrics(case, frames, truth):
    """Everything for one case: its segments, or (idle) its false readings, or (free) steadiness."""
    pairs = join(frames, truth)
    out = {"id": case["id"], "dataset": case["dataset"], "family": case["family"], "value": case["value"],
           "file": case["file"], "frames": len(frames)}
    if case["family"] == "idle":
        return idle_metrics(frames, out)
    named = [s for s in case["segments"] if s["midi"] > 0 and s["truth"] != "disputed"]
    out["segments"] = [segment_metrics(pairs, s) for s in named]
    out["disputed"] = sum(1 for s in case["segments"] if s["truth"] == "disputed")
    if not named:
        out["free"] = free_metrics(pairs)
    return out
