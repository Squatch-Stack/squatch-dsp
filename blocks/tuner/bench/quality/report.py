# SPDX-License-Identifier: MIT
"""The scorecard's outputs: scorecard.json, scorecard.md and index.html (inline SVG, no fetches).

Platforms are columns: every frames-<platform>-b<block> set in the runs, plus the phone and the
S3 probe, which stay "pending" until their logs are scored with `score.py file`.
"""
import html
import json
import os

PLATFORM_COLUMNS = ["mac-b16", "mac-b64", "pi-b16", "pi-b64", "pi-live-b64", "tunercsv-b64", "iphone14-b64", "s3-b64"]
HEADLINE = [("wobble_vs_truth_p95", "added wobble p95 (c)", 2), ("wobble_p95", "wobble p95 (c)", 2), ("jumps_per_min", "jumps/min", 2), ("pp_steady_p95", "p-p steady p95 (c)", 2), ("delta_p90", "Δ frame p90 (c)", 3),
            ("flips_per_s", "flips/s", 2), ("wrong_rate", "wrong note", "%"), ("err_median", "abs err median (c)", 3),
            ("err_p95", "abs err p95 (c)", 2), ("note_err_median", "note err median (c)", 2), ("coverage", "coverage", "%"), ("settle_1c_median", "settle ±1 c (s)", 2)]
DATASET_ROWS = [("synthetic", "core"), ("synthetic", "stress"), ("synthetic", "moving"), ("egfxset", "core"), ("guitar-techs", "core"),
                ("owner", "core"), ("owner-line", "core"), ("owner", "free"), ("owner-line", "free"), ("regression", "regression"),
                ("idmt", "core"), ("nsynth", "core"), ("nsynth", "outside"), ("guitarset", "moving")]


def fmt(v, kind):
    if v is None:
        return "–"
    if kind == "%":
        return f"{100 * v:.2f}%"
    return f"{v:.{kind}f}"


def mark(ok):
    return "" if ok is None else (" ✓" if ok else " ✗ FAIL")


def group(board, platform, *key):
    return board["platforms"].get(platform, {}).get("groups", {}).get("|".join(key))


def platforms_present(board):
    return [p for p in PLATFORM_COLUMNS if p in board["platforms"]] + sorted(p for p in board["platforms"] if p not in PLATFORM_COLUMNS)


def steady_checks(dataset):
    """Synthetic: what the tuner adds to the exact truth. Real: the shown number's own wobble, an
    upper bound that includes the string's real waver (the reference contour is too coarse to
    subtract at 0.1 c), plus leaps, frame-to-frame change and flips."""
    first = "wobble_vs_truth_p95" if dataset == "synthetic" else "wobble_p95"
    return (first, "jumps_per_min", "delta_p90", "flips_per_s")


def verdict(board, platform):
    """Steadiness and accuracy PASS/FAIL over the core rows of every dataset."""
    steady = all(r["checks"].get(k, True) for (d, t), r in core_rows(board, platform) for k in steady_checks(d))
    accurate = all(r["checks"].get(k, True) for (d, t), r in core_rows(board, platform) for k in accuracy_checks(d))
    return steady, accurate


def accuracy_checks(dataset):
    """Synthetic truth is exact: judge the error itself. A real note's reference is good to its
    own drift (cents on a real string), so real sets are judged on the note and the median bias."""
    return ("wrong_rate", "err_p95", "err_median") if dataset == "synthetic" else ("wrong_rate", "note_err_median")


def core_rows(board, platform):
    out = []
    for d, t in DATASET_ROWS:
        g = group(board, platform, "dataset", d, t)
        if t == "core" and g:
            out.append(((d, t), g))
    return out


def pending_platforms(board):
    """The phone and the S3 probe, until a run of theirs is in the scorecard."""
    return [p for p in ("iphone14", "s3") if not any(k.startswith(p) for k in board["platforms"])]


def both(board):
    """Platforms scored now and in the baseline, in column order."""
    base = board.get("baseline", {}).get("platforms", {})
    return [p for p in platforms_present(board) if p in base]


def md_before_after_verdict(board):
    out = ["| platform | steadiness before | steadiness after | accuracy before | accuracy after |", "|---|---|---|---|---|"]
    for p in both(board):
        (sb, ab), (sa, aa) = verdict(board["baseline"], p), verdict(board, p)
        word = lambda ok: "PASS" if ok else "FAIL"  # noqa: E731
        out.append(f"| {p} | {word(sb)} | {word(sa)} | {word(ab)} | {word(aa)} |")
    return out


def md_before_after(board, platform):
    """Every headline measure of every dataset row as before -> after, the after marked against its threshold."""
    head = "| dataset | tier | " + " | ".join(h for _, h, _ in HEADLINE) + " |"
    out = [head, "|" + "---|" * (2 + len(HEADLINE))]
    for d, t in DATASET_ROWS:
        g, b = group(board, platform, "dataset", d, t), group(board["baseline"], platform, "dataset", d, t)
        if not g:
            continue
        cells = [f"{fmt((b or {}).get(k), kind)} → {fmt(g.get(k), kind)}{mark(g['checks'].get(k))}" for k, _, kind in HEADLINE]
        out.append(f"| {d} | {t} | " + " | ".join(cells) + " |")
    return out


def md_headline(board):
    out = ["| platform | cases scored | steadiness | accuracy |", "|---|--:|---|---|"]
    for p in platforms_present(board):
        s, a = verdict(board, p)
        n = len(board["platforms"][p]["results"])
        partial = " (partial: the phone pack)" if n < 100 else ""
        out.append(f"| {p} | {n}{partial} | {'PASS' if s else 'FAIL'} | {'PASS' if a else 'FAIL'} |")
    out += [f"| {p} | 0 | pending | pending |" for p in pending_platforms(board)]
    return out


def md_dataset_table(board, platform):
    head = "| dataset | tier | cases | notes | " + " | ".join(h for _, h, _ in HEADLINE) + " |"
    out = [head, "|" + "---|" * (4 + len(HEADLINE))]
    for d, t in DATASET_ROWS:
        g = group(board, platform, "dataset", d, t)
        if not g:
            continue
        cells = [fmt(g.get(k), kind) + mark(g["checks"].get(k)) for k, _, kind in HEADLINE]
        out.append(f"| {d} | {t} | {g['cases']} | {g['segments']} | " + " | ".join(cells) + " |")
    return out


def md_conditions(board, platform):
    out = ["| family | condition | cases | wobble p95 | abs err p95 | wrong note | coverage | settle ±1 c | never ±0.1 c |", "|---|---|--:|--:|--:|--:|--:|--:|--:|"]
    rows = sorted((k.split("|")[1:], v) for k, v in board["platforms"][platform]["groups"].items() if k.startswith("condition|"))
    for (fam, val), g in rows:
        out.append(f"| {fam} | {val} | {g['cases']} | {fmt(g['wobble_p95'], 2)}{mark(g['checks'].get('wobble_p95'))} | "
                   f"{fmt(g['err_p95'], 2)}{mark(g['checks'].get('err_p95'))} | {fmt(g['wrong_rate'], '%')} | {fmt(g['coverage'], '%')} | "
                   f"{fmt(g['settle_1c_median'], 2)} | {g['never_01c']}/{g['truth_hz']} |")
    return out


def md_owner(board, platform):
    out = ["| take | notes | disputed | added wobble p95 | wobble p95 | wobble max | jumps | Δ p90 | flips/s | wrong note | note err median (c) | false frames |",
           "|---|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|"]
    for k, g in sorted(board["platforms"][platform]["groups"].items()):
        if not k.startswith("take|"):
            continue
        out.append(f"| {k.split('|', 1)[1]} | {g['segments']} | {g['disputed']} | {fmt(g['wobble_vs_truth_p95'], 2)} | {fmt(g['wobble_p95'], 2)} | {fmt(g['wobble_max'], 2)} | "
                   f"{g['jumps']} | {fmt(g['delta_p90'], 3)} | "
                   f"{fmt(g['flips_per_s'], 2)} | {fmt(g['wrong_rate'], '%')} | {fmt(g['note_err_median'], 2)} | {g['false_frames']} |")
    return out


def md_worst(board, platform):
    out = []
    for kind, items in board["platforms"][platform]["worst"].items():
        if not items:
            continue
        out += [f"**Worst: {kind}**", "", "| value | case | file | at (s) |", "|--:|---|---|--:|"]
        for w in items[:8]:
            v = f"{100 * w['value']:.1f}%" if kind == "wrong note" else f"{w['value']:.2f}"
            out.append(f"| {v} | {w['id']} | {w['file'] or '(synthetic)'} | {w['t']:.2f} |" if w["t"] is not None else f"| {v} | {w['id']} | {w['file']} | – |")
        out.append("")
    return out


def md_comparisons(board):
    out = ["| A | B | cases | rows | differing cases | differing rows | max live-cents gap | first differences (case @ s: A / B) |",
           "|---|---|--:|--:|--:|--:|--:|---|"]
    for c in board["comparisons"]:
        first = "; ".join(f"{d['id']} @ {d['first_t']:.2f}: {d['a']} / {d['b']}" for d in c["diffs"][:3]) or "none"
        out.append(f"| {c['a']} | {c['b']} | {c['cases']} | {c['rows']} | {c['differing_cases']} | {c['differing_rows']} | {c['max_cents_gap']:.4f} | {first} |")
    return out


def write_markdown(board, path, notes):
    import breakdowns_md
    lines = ["# Squatch Tuner quality scorecard", "", "\n\n".join(notes), "", "## Verdict", "", *md_headline(board), "", *breakdowns_md.md_top(board)]
    if both(board):
        lines += ["## Before and after", "", *md_before_after_verdict(board), ""]
        for p in both(board):
            lines += [f"### {p}: before → after", "", *md_before_after(board, p), ""]
    lines += ["## Thresholds", "", "| measure | pass if |", "|---|---|"]
    lines += [f"| {k} | {'≥' if k == 'coverage' else '≤'} {v} |" for k, v in board["thresholds"].items()]
    for p in platforms_present(board):
        lines += ["", f"## {p}: per dataset", "", *md_dataset_table(board, p), "", f"### {p}: owner takes", "", *md_owner(board, p)]
        lines += ["", f"### {p}: synthetic conditions", "", *md_conditions(board, p), "", f"### {p}: worst cases", "", *md_worst(board, p)]
    lines += ["", "## Platforms compared frame by frame", "", *md_comparisons(board), ""]
    lines += breakdowns_md.md_sections(board)
    with open(path, "w") as f:
        f.write("\n".join(lines))


def compact(board):
    """scorecard.json without the per-frame lists (they stay in the run directories)."""
    slim = {"thresholds": board["thresholds"], "comparisons": board["comparisons"], "platforms": {}}
    for p, v in board["platforms"].items():
        slim["platforms"][p] = {"groups": v["groups"], "worst": v["worst"], "verdict": dict(zip(("steadiness", "accuracy"), verdict(board, p)))}
    if "analysis" in board:
        slim["analysis"] = board["analysis"]
    if "baseline" in board:
        base = board["baseline"]
        slim["baseline"] = {p: {"groups": v["groups"], "verdict": dict(zip(("steadiness", "accuracy"), verdict(base, p)))}
                            for p, v in base["platforms"].items()}
    return slim


def write_all(board, out, notes=()):
    with open(os.path.join(out, "scorecard.json"), "w") as f:
        json.dump(compact(board), f, indent=1)
    write_markdown(board, os.path.join(out, "scorecard.md"), list(notes))
    import report_html
    report_html.write(board, os.path.join(out, "index.html"), list(notes))


def write_manifest(pack):
    """manifest.json for the phone pack: every WAV with its truth, case and reference frames."""
    items = []
    for name in sorted(os.listdir(pack)):
        if not name.endswith(".case.json"):
            continue
        cid = name[:-len(".case.json")]
        with open(os.path.join(pack, name)) as f:
            case = json.load(f)
        size = os.path.getsize(os.path.join(pack, cid + ".wav"))
        items.append({"id": cid, "wav": cid + ".wav", "bytes": size, "seconds": case["seconds"], "dataset": case["dataset"],
                      "truth": cid + ".truth.csv", "case": name, "reference_frames": sorted(x for x in os.listdir(pack) if x.startswith(cid + ".frames-")),
                      "source": case["source"], "segments": len(case["segments"])})
    with open(os.path.join(pack, "manifest.json"), "w") as f:
        json.dump({"format": FRAME_SPEC, "files": items, "total_bytes": sum(i["bytes"] for i in items)}, f, indent=1)
    print(f"{len(items)} files, {sum(i['bytes'] for i in items) / 1e6:.1f} MB")


FRAME_SPEC = {
    "frames_csv": "t_ms,note,octave,cents,hz,valid,shown_cents",
    "t_ms": "audio position of the reading shown, ms from the start of the WAV (TunerSnapshot.samples / rate * 1000), 3 decimals",
    "note,octave": "the note on screen, live or held (e.g. C#,3); both empty when no note is shown",
    "cents": "live reading's cents vs A4 = 440 (DisplayReading.cents), 4 decimals; empty when not reading",
    "hz": "live steadyHz, 4 decimals; empty when not reading",
    "valid": "1 while the tuner reads, else 0",
    "shown_cents": "the number on screen, one decimal (+0.3 -> 0.3); empty for --.- or when no note is shown",
    "poll": "one row per display read, 30 per second, after the audio block that crosses each 1/30 s of file time",
    "a4": 440,
    "truth_csv": "t_ms,truth_hz,truth_midi,steady,segment (truth_hz 0 = unknown; steady = moved < 0.2 c over the last 300 ms)",
    "compare": "score.py diff <mac frames> <device frames>: any row that differs in note, valid or shown_cents is a bug",
    "score": "score.py file --case X.case.json --truth X.truth.csv --frames <device frames>",
}


def esc(s):
    return html.escape(str(s))
