# SPDX-License-Identifier: MIT
"""The scorecard's breakdown sections as Markdown lines and as HTML (standard library only): the
verdict per set, the breakdowns, the pickup question, tracking over time, the new sets' worst notes
and the data licences, from board["analysis"] (breakdowns.augment)."""
import re

from breakdowns import INTERNAL_ONLY, NEW_SETS
from tracking import MOVING_CENTS, STILL_CENTS


def fmt(v, kind):
    import report as R
    return R.fmt(v, kind)


def _tag(d):
    return " (internal only)" if d in INTERNAL_ONLY else ""


def reference_platform(board):
    bd = board.get("analysis", {}).get("breakdowns", {})
    return next((p for p in ("mac-b64", "mac-b16") if p in bd), next(iter(bd), None))


# ---------------------------------------------------------------------------------------- per set --

WORD = {True: "PASS", False: "FAIL"}


def _set_cell(lst, d, t, p):
    """(cell, found) for one dataset row on one platform."""
    r = next((x for x in lst if x["dataset"] == d and x["tier"] == t), None)
    if not r:
        return ("pending" if d in NEW_SETS and p.startswith("pi") else "–"), False
    why = f" ({', '.join(r['failed'])})" if r["failed"] else ""
    return f"steady {WORD[r['steadiness']]}, accurate {WORD[r['accuracy']]}{why}", True


def md_per_set(board):
    import report as R
    per = board["analysis"]["per_set"]
    out = ["| dataset | tier | " + " | ".join(per) + " |", "|---|---|" + "---|" * len(per)]
    for d, t in R.DATASET_ROWS:
        cells = [_set_cell(lst, d, t, p) for p, lst in per.items()]
        if any(found for _, found in cells):
            out.append(f"| {d}{_tag(d)} | {t} | " + " | ".join(c for c, _ in cells) + " |")
    return out


# ------------------------------------------------------------------------------------- breakdowns --

BD_COLS = [("segments", "notes", None), ("wrong_rate", "wrong note", "%"), ("octave_rate", "octave", "%"), ("first_correct_median", "first right (s)", 2),
           ("coverage", "coverage", "%"), ("settle_1c_median", "settle ±1 c (s)", 2), ("note_err_median", "note err med (c)", 2),
           ("wobble_p95", "wobble p95 (c)", 2), ("jumps_per_min", "jumps/min", 2)]
FRET_ORDER = {"open": 0, "1-4": 1, "5-9": 2, "10-14": 3, "15-24": 4}


def sort_key(k):
    tail = k.split("|", 2)[2]
    m = re.match(r"[sp]?(\d+)", tail)
    return (FRET_ORDER.get(tail, 9), int(m.group(1)) if m else 0, tail)


def _bd_cell(g, c, kind):
    return str(g[c]) if kind is None else fmt(g.get(c), kind)


def md_breakdown(board, p, dataset):
    rows = {k: v for k, v in board["analysis"]["breakdowns"].get(p, {}).items() if k.startswith(dataset + "|")}
    axes = list(dict.fromkeys(k.split("|")[1] for k in rows))
    out = []
    for ax in axes:
        out += [f"| {ax} | " + " | ".join(h for _, h, _ in BD_COLS) + " |", "|---|" + "--:|" * len(BD_COLS)]
        for k in sorted((k for k in rows if k.split("|")[1] == ax), key=sort_key):
            out.append(f"| {k.split('|', 2)[2]} | " + " | ".join(_bd_cell(rows[k], c, kind) for c, _, kind in BD_COLS) + " |")
        out.append("")
    return out


# ----------------------------------------------------------------------------------------- pickup --

def _pickup_medians(res):
    pos = res["positions"]
    out = ["| measure | notes | Friedman p | Kendall's W | " + " | ".join(f"{x} median" for x in pos) + " |", "|---|--:|--:|--:|" + "--:|" * len(pos)]
    for key, m in res["measures"].items():
        kind = "%" if key in ("wrong", "coverage") else 3
        pv = "–" if m["p"] is None else f"{m['p']:.3g}"
        wv = "–" if m["W"] is None else f"{m['W']:.3f}"
        out.append(f"| {m['label']} | {m['complete']} | {pv} | {wv} | " + " | ".join(fmt(m["median"][x], kind) for x in pos) + " |")
    return out


def _paired_cell(v, key):
    if not v:
        return "–"
    scale, unit = (100.0, " pp") if key in ("wrong", "coverage") else (1.0, "")
    return f"{scale * v['median']:+.3g}{unit} [{scale * v['lo']:+.3g}, {scale * v['hi']:+.3g}]"


def _pickup_paired(res):
    others = [x for x in res["positions"] if x != res["reference"]]
    out = ["", f"Paired difference from {res['reference']} (median, bootstrap 95% interval):", "",
           "| measure | " + " | ".join(others) + " |", "|---|" + "--:|" * (len(res["positions"]) - 1)]
    for key, m in res["measures"].items():
        out.append(f"| {m['label']} | " + " | ".join(_paired_cell(m["vs_ref"].get(x), key) for x in others) + " |")
    return out


def md_pickup(board, p):
    out = []
    for d, res in board["analysis"]["pickup"].get(p, {}).items():
        out += [f"**{d}{_tag(d)}**: {res['notes']} notes played at every position, paired by string and fret. Reference: {res['reference']}.", ""]
        out += _pickup_medians(res) + _pickup_paired(res)
        out += ["", "| position | live frames | wrong frames | of which octave |", "|---|--:|--:|--:|"]
        out += [f"| {x} | {res['live_frames'][x]} | {res['wrong_frames'][x]} | {res['octave_frames'][x]} |" for x in res["positions"]]
        out.append("")
    return out


# --------------------------------------------------------------------------------------- tracking --

def _tracking_bins(t):
    out = ["| time since the note began | frames | right note, live | wrong note (of live) | octave (of live) | abs err median (c) | p90 (c) |",
           "|---|--:|--:|--:|--:|--:|--:|"]
    for b in t["bins"]:
        span = f"{b['from']:g}-{b['to']:g} s" if b["to"] else f"≥ {b['from']:g} s"
        out.append(f"| {span} | {b['frames']} | {fmt(b['coverage'], '%')} | {fmt(b['wrong'], '%')} | {fmt(b['octave'], '%')} | {fmt(b['median'], 2)} | {fmt(b['p90'], 2)} |")
    return out


def _movement_row(label, s):
    return f"| {label} | {s['n']} | {fmt(s['median'], 2)} | {fmt(s['p90'], 2)} | {fmt(s['p95'], 2)} |"


def _tracking_movement(t):
    return ["", "Live frames on the right note, by how the annotated pitch moved over the last 100 ms:", "",
            "| the string | frames | abs err median (c) | p90 (c) | p95 (c) |", "|---|--:|--:|--:|--:|",
            _movement_row(f"moving smoothly, ≥ {MOVING_CENTS:g} c in one direction (bends, vibrato)", t["moving"]),
            _movement_row(f"a step, ≥ {MOVING_CENTS:g} c mostly in one frame (legato note changes, contour jumps)", t["steps"]),
            _movement_row(f"still, < {STILL_CENTS:g} c", t["still"]), "",
            "While the string moves smoothly, the shown pitch against the contour some time earlier (the smallest error is the display's lag):", "",
            "| contour this long ago | " + " | ".join(f"{x['ms']} ms" for x in t["lag"]) + " |", "|---|" + "--:|" * len(t["lag"]),
            "| abs err median (c) | " + " | ".join(fmt(x["median"], 2) for x in t["lag"]) + " |", "",
            f"Best fit: the display follows a moving string about {t['lag_ms']} ms late." if t.get("lag_ms") is not None else "", ""]


def _tracking_lengths(t):
    out = ["| note length | notes | polled at least once | never read (no live frame on the right note) | right-note share of its frames (median) |",
           "|---|--:|--:|--:|--:|"]
    for b in t["by_length"]:
        out.append(f"| {b['bin']} | {b['notes']} | {b.get('polled', '–')} | {fmt(b['never_read'], '%')} | {fmt(b['coverage'], '%')} |")
    return out


def _tracking_causes(t):
    total = sum(c["frames"] for c in t.get("causes", []))
    if not total:
        return []
    out = ["", f"Why a live frame shows the wrong note ({total} frames from 0.1 s into a note):", "", "| cause | when | frames | share |", "|---|---|--:|--:|"]
    return out + [f"| {c['cause']} | {c['when']} | {c['frames']} | {fmt(c['frames'] / total, '%')} |" for c in t["causes"]]


def md_tracking(board, p):
    t = board["analysis"]["tracking"].get(p)
    if not t:
        return []
    av = t["annotation_vs_reference"]
    return (_tracking_bins(t) + _tracking_movement(t) + _tracking_lengths(t) + _tracking_causes(t) +
            ["", f"The annotation's own accuracy: on {av['n']} notes long enough for the offline reference, the annotated contour's median over "
                 f"0.5-1.7 s is {fmt(av['median'], 2)} c from the reference (p90 {fmt(av['p90'], 2)} c).", ""])


# ------------------------------------------------------------------------------------------ worst --

def _worst_value(w, kind):
    return f"{w['value']} frames ({100 * w['share']:.0f}%)" if kind == "wrong note" else f"{w['value']:.2f}"


def _worst_table(d, kind, items):
    out = [f"**{d}{_tag(d)}, worst {kind}**", "", "| value | case | file | at (s) |", "|--:|---|---|--:|"]
    out += [f"| {_worst_value(w, kind)} | {w['id']} | {w['file']} | {'–' if w['t'] is None else format(w['t'], '.2f')} |" for w in items[:8]]
    return out + [""]


def _worst_tracking(t):
    if not t or not t["worst"]:
        return []
    out = ["**guitarset, worst tracking** (median |error| against the contour over one note)", "",
           "| value (c) | case | file | note at (s) | largest at (s) |", "|--:|---|---|--:|--:|"]
    out += [f"| {w['value']:.2f} | {w['id']} | {w['file']} | {w['onset']:.2f} | {w['t']:.2f} |" for w in t["worst"][:8]]
    return out + [""]


def md_worst_new(board, p):
    out = []
    for d, kinds in board["analysis"]["worst"].get(p, {}).items():
        for kind, items in kinds.items():
            out += _worst_table(d, kind, items) if items else []
    return out + _worst_tracking(board["analysis"]["tracking"].get(p))


# --------------------------------------------------------------------------------------- sections --

def md_licences(board):
    a = board["analysis"]
    used = {c["dataset"].split("-line")[0] for c in board["cases"].values()}
    out = ["| set | licence | DOI / source | attribution | may appear in anything shipped or public |", "|---|---|---|---|---|"]
    return out + [f"| {L['name']} | {L['licence']} | {L['doi']} | {L['attribution']} | {L['public']} |" for L in a["licences"] if L["set"] in used]


def md_top(board):
    """The per-set verdict, for the top of the scorecard."""
    if "analysis" not in board:
        return []
    return ["## Verdict per set", "", "Each dataset row on its own, on every platform (the overall verdict is every core row together; "
            "GuitarSet is real playing, with bends and vibrato, so it is reported as a moving set, like the synthetic vibrato and bends). "
            "The Pi columns for the three new sets are pending: the Pi was in use for the live tuner.", "", *md_per_set(board), ""]


def _md_breakdowns(board, p):
    nt = board["analysis"]["nsynth_tuning"]
    lines = [f"## Breakdowns ({p})", "",
             f"NSynth's own tuning: the offline reference puts its {nt['notes']} fitted notes a median {fmt(nt['median_abs'], 2)} c from their "
             f"MIDI labels (p90 {fmt(nt['p90_abs'], 2)} c; {fmt(nt['over_10c'], '%')} beyond 10 c, {fmt(nt['over_25c'], '%')} beyond 25 c); "
             f"{nt['no_reference']} notes had no reference (too short or too quiet), and are scored for the note only.", ""]
    for d in ("egfxset", "idmt", "guitarset", "nsynth", "guitar-techs"):
        body = md_breakdown(board, p, d)
        lines += [f"### {d}{_tag(d)}", "", *body] if body else []
    return lines


def md_sections(board):
    """Breakdowns, the pickup question, tracking over time, the new sets' worst cases, licences."""
    if "analysis" not in board:
        return []
    p = reference_platform(board)
    lines = []
    if p:
        lines += _md_breakdowns(board, p)
        lines += [f"## Does pickup position matter? ({p})", "", *md_pickup(board, p)]
        lines += [f"## Following pitch over time: GuitarSet ({p})", "", *md_tracking(board, p)]
        lines += [f"## Worst cases in the new sets ({p})", "", *md_worst_new(board, p)]
    return lines + ["## Data licences", "", "Every set this scorecard used. The full attribution text for each (all authors, title, "
                    "source) is in the DI corpus folder's LICENSES.md.", "", *md_licences(board), "",
                    "Nothing from any dataset is in the repository. IDMT-SMT-Guitar (CC BY-NC-ND 4.0) is used for internal testing only: "
                    "its rows above are internal results and must be removed before this scorecard is shared outside; no audio derived "
                    "from it is written anywhere (the phone pack refuses it).", ""]


# ------------------------------------------------------------------------------------------- html --

def _para(x):
    import report_html as H
    return re.sub(r"\*\*(.+?)\*\*", r"<b>\1</b>", H.R.esc(x))


def _chunk_html(chunk):
    import report_html as H
    return H.md_table_html(chunk) if chunk[0].startswith("|") else "".join(f"<p>{_para(x)}</p>" for x in chunk)


def _heading_html(line):
    import report_html as H
    level = 2 if line.startswith("## ") else 3
    return f"<h{level}>{H.R.esc(line.lstrip('#').strip())}</h{level}>"


def _blocks(md):
    """md_* lines as ("heading", line) and ("chunk", [lines]) blocks: a table or a run of paragraphs."""
    chunk = []
    for line in md:
        if line.startswith("#") or not line.strip() or (chunk and chunk[-1].startswith("|") != line.startswith("|")):
            if chunk:
                yield "chunk", chunk
            chunk = []
        if line.startswith("#"):
            yield "heading", line
        elif line.strip():
            chunk.append(line)
    if chunk:
        yield "chunk", chunk


def md_to_html(md):
    """Headings, tables and paragraphs of md_* lines as HTML (tables through report_html)."""
    return "".join(_heading_html(x) if kind == "heading" else _chunk_html(x) for kind, x in _blocks(md))


def html_top(board):
    return md_to_html(md_top(board)) if "analysis" in board else ""


def html_sections(board):
    """The sections after the tables, with the GuitarSet traces."""
    import report_html as H
    if "analysis" not in board:
        return ""
    out = md_to_html(md_sections(board))
    for cid, tr in board["analysis"].get("traces", {}).items():
        out += (f'<h3>Trace: {H.R.esc(cid)}</h3><p class="muted">{H.R.esc(tr["why"])}</p><div class="legend muted"><span><i style="background:var(--s1)">'
                f'</i>shown number (c)</span><span><i style="background:var(--s2)"></i>annotated truth (c)</span></div>' + H.trace(tr, cid))
    return out
