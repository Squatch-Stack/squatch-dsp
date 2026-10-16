# SPDX-License-Identifier: MIT
"""Tracking over time on GuitarSet, for the scorecard's breakdowns (standard library only): every
live frame against the annotated contour, by time since the note began; moving (bends, vibrato)
against still; the lag that best explains the display on a moving string; how often notes of each
length are read at all; and why a live frame shows the wrong note."""
from statistics import median

import metrics as M
from stats import q, summary

TAU_BINS = [(0.0, 0.1), (0.1, 0.2), (0.2, 0.3), (0.3, 0.5), (0.5, 1.0), (1.0, 2.0), (2.0, 1e9)]
DUR_BINS = [(0.0, 0.15), (0.15, 0.3), (0.3, 0.6), (0.6, 1.2), (1.2, 1e9)]
MOVING_CENTS = 5.0   # truth moved this much over the last 100 ms: a bend, vibrato or slide
STILL_CENTS = 1.0    # ... less than this: still
LAG_FRAMES = 10      # lags tried: 0 .. 9 frames (0 .. 300 ms at 30 Hz)


def dur_bin(d):
    for lo, hi in DUR_BINS:
        if lo <= d < hi:
            return f"{lo:g}-{hi:g} s" if hi < 1e8 else f"≥ {lo:g} s"
    return "?"


def tau_bin(tau):
    """The TAU_BINS index for a time since the onset, or None before it."""
    if tau < 0:
        return None
    return next(i for i, (lo, hi) in enumerate(TAU_BINS) if lo <= tau < hi)


def movement(tr, j, seg):
    """How the truth moved over the 100 ms up to frame j: "smooth" (5 c or more, in one direction,
    no single frame carrying 60% of it: bends, vibrato), "step" (5 c or more, mostly in one frame:
    a legato note change or a jump in the contour), "still" (under 1 c), or None."""
    if j < 3:
        return None
    w = tr[j - 3:j + 1]
    if any(x["seg"] != seg or x["hz"] <= 0 for x in w):
        return None
    d = [M.cents(b["hz"], a["hz"]) for a, b in zip(w, w[1:])]
    return _movement_kind(d)


def _movement_kind(d):
    total, peak = abs(sum(d)), max(abs(x) for x in d)
    if total < STILL_CENTS and peak < STILL_CENTS:
        return "still"
    if total < MOVING_CENTS:
        return None
    if peak > 0.6 * total:
        return "step"
    return "smooth" if all(x * sum(d) >= 0 for x in d) else None


def wrong_cause(shown, truth, segs, si):
    """Why a live frame shows the wrong note, as far as the truth tells."""
    prev = next((segs[k]["midi"] for k in range(si - 1, -1, -1) if segs[k]["midi"] > 0), None)
    d = shown - truth
    if prev is not None and shown == prev and d % 12 != 0:
        return "still the string's previous note"
    if d % 12 == 0:
        return "octave up" if d > 0 else "octave down"
    if abs(d) <= 2:
        return "1-2 semitones (bend, slide, or at a note boundary)"
    return "another note (bleed from other strings)"


def _share(n, of):
    return n / of if of else None


class Tracking:
    """Accumulates every GuitarSet case's frames; result() is the scorecard's tracking section."""

    def __init__(self):
        self.bins = [{"frames": 0, "live": 0, "right": 0, "wrong": 0, "octave": 0, "err": []} for _ in TAU_BINS]
        self.kinds = {"smooth": [], "step": [], "still": []}
        self.causes = {}
        self.lag = [[] for _ in range(LAG_FRAMES)]
        self.per_note, self.worst_err, self.annot = [], [], []

    # -- one case --

    def case(self, cid, c, frames, tr):
        self.segs, self.tr = c["segments"], tr
        self.idx = {id(t): i for i, t in enumerate(tr)}
        self.notes = {}
        for f, t in M.join(frames, tr):
            if t is not None and t["seg"] >= 0 and t["midi"] > 0:
                self._frame(f, t)
        self._notes(cid, c, self.notes)
        self._annotation(self.segs, tr)

    def _frame(self, f, t):
        tau = f["t"] - self.segs[t["seg"]]["onset"]
        b = tau_bin(tau)
        if b is None:
            return
        nb = self.bins[b]
        nb["frames"] += 1
        nf = self.notes.setdefault(t["seg"], {"frames": 0, "right": 0, "err": [], "worst": (0.0, None)})
        nf["frames"] += 1
        if not f["valid"]:
            return
        nb["live"] += 1
        if f["midi"] != t["midi"]:
            self._wrong(nb, f, t, tau)
            return
        nb["right"] += 1
        nf["right"] += 1
        if t["hz"] > 0 and f["hz"]:
            self._error(nb, nf, f, t)

    def _wrong(self, nb, f, t, tau):
        nb["wrong"] += 1
        nb["octave"] += (f["midi"] - t["midi"]) % 12 == 0
        if tau >= 0.1:
            k = (wrong_cause(f["midi"], t["midi"], self.segs, t["seg"]), "0.1-0.3 s" if tau < 0.3 else "≥ 0.3 s")
            self.causes[k] = self.causes.get(k, 0) + 1

    def _error(self, nb, nf, f, t):
        e = abs(M.cents(f["hz"], t["hz"]))
        nb["err"].append(e)
        nf["err"].append(e)
        if e > nf["worst"][0]:
            nf["worst"] = (e, f["t"])
        j = self.idx[id(t)]
        kind = movement(self.tr, j, t["seg"])
        if kind in self.kinds:
            self.kinds[kind].append(e)
        if kind == "smooth":
            self._lags(f, t, j)

    def _lags(self, f, t, j):
        for k in range(LAG_FRAMES):
            u = self.tr[j - k] if j >= k else None
            if u is not None and u["seg"] == t["seg"] and u["hz"] > 0:
                self.lag[k].append(abs(M.cents(f["hz"], u["hz"])))

    def _notes(self, cid, c, notes):
        segs = c["segments"]
        for si, nf in notes.items():
            s = segs[si]
            self.per_note.append({"dur": s["end"] - s["onset"], "read": nf["right"] > 0, "cover": _share(nf["right"], nf["frames"])})
            if len(nf["err"]) >= 5:
                self.worst_err.append({"value": median(nf["err"]), "id": cid, "file": c["file"], "t": nf["worst"][1], "onset": s["onset"]})
        for si, s in enumerate(segs):  # notes too short to be polled even once count as not read
            if si not in notes and s["midi"] > 0:
                self.per_note.append({"dur": s["end"] - s["onset"], "read": False, "cover": None})

    def _annotation(self, segs, tr):
        """The annotation against the offline reference, where a note was long enough for both."""
        for si, s in enumerate(segs):
            if s.get("ref_hz", 0) <= 0:
                continue
            hz = [t["hz"] for t in tr if t["seg"] == si and t["hz"] > 0 and 0.5 <= t["t"] - s["onset"] <= 1.7]
            if len(hz) >= 5:
                self.annot.append(M.cents(median(hz), s["ref_hz"]))

    # -- the section --

    def result(self):
        k = self.kinds
        lag = [{"ms": round(1000 * i / 30), "median": q(v, 0.5), "n": len(v)} for i, v in enumerate(self.lag)]
        best = min((x for x in lag if x["median"] is not None), key=lambda x: x["median"], default=None)
        return {"bins": self._bins(), "moving": summary(k["smooth"]), "steps": summary(k["step"]), "still": summary(k["still"]),
                "lag": lag, "by_length": self._by_length(), "annotation_vs_reference": summary([abs(a) for a in self.annot]),
                "causes": [{"cause": c[0], "when": c[1], "frames": v} for c, v in sorted(self.causes.items(), key=lambda kv: -kv[1])],
                "lag_ms": best["ms"] if best else None, "worst": sorted(self.worst_err, key=lambda w: -w["value"])[:12]}

    def _bins(self):
        out = []
        for (lo, hi), b in zip(TAU_BINS, self.bins):
            out.append({"from": lo, "to": hi if hi < 1e8 else None, "frames": b["frames"], "coverage": _share(b["right"], b["frames"]),
                        "wrong": _share(b["wrong"], b["live"]), "octave": _share(b["octave"], b["live"]),
                        **{k: v for k, v in summary(b["err"]).items() if k != "n"}, "n_err": len(b["err"])})
        return out

    def _by_length(self):
        out = []
        for lo, hi in DUR_BINS:
            ns = [n for n in self.per_note if lo <= n["dur"] < hi]
            covers = [n["cover"] for n in ns if n["cover"] is not None]
            out.append({"bin": dur_bin(lo), "notes": len(ns), "never_read": _share(sum(1 for n in ns if not n["read"]), len(ns)),
                        "polled": len(covers), "coverage": q(covers, 0.5)})
        return out


def tracking(frames_by_case, truth, cases):
    """GuitarSet: every live frame against the annotated contour, by time since the note began."""
    acc = Tracking()
    for cid, frames in frames_by_case.items():
        c = cases.get(cid)
        if c and c["dataset"] == "guitarset":
            acc.case(cid, c, frames, truth.get(cid, []))
    return acc.result()
