# SPDX-License-Identifier: MIT
"""Comparing platforms: two logs of the same audio, frame by frame, and device logs gathered into
a run the scorecard can read. Standard library only."""
import json
import os

import metrics as M

GRID = 5e-5  # s: frames further apart than this are not the same frame


def same_grid(fa, fb):
    return all(abs(x["t"] - y["t"]) <= GRID for x, y in zip(fa[:3], fb[:3]))


def case_diff(cid, fa, fb):
    """One case: rows that differ on screen, the largest live-cents gap, a length mismatch."""
    pairs = list(zip(fa, fb))
    bad = [(x, y) for x, y in pairs if key(x) != key(y) or abs(x["t"] - y["t"]) > GRID]
    gaps = [abs(x["cents"] - y["cents"]) for x, y in pairs if x["cents"] is not None and y["cents"] is not None]
    d = {"id": cid, "rows": len(pairs), "frames": len(bad), "gap": max(gaps, default=0.0), "len": [len(fa), len(fb)]}
    if bad:
        d.update(first_t=bad[0][0]["t"], a=row_text(bad[0][0]), b=row_text(bad[0][1]))
    return d


def compare(a, b):
    """Frame-by-frame differences between two platforms' logs of the same cases: rows (by position)
    whose note, validity or shown number differ, the first such time, the largest live-cents gap,
    and cases whose logs differ in length (a take cut short on one side) reported apart."""
    common = sorted(set(a) & set(b))
    if common and not same_grid(a[common[0]], b[common[0]]):
        return {"cases": len(common), "grid": "different frame times: compared through a replay instead"}
    return summary(len(common), [case_diff(cid, a[cid], b[cid]) for cid in common])


def summary(cases, per):
    diffs = [{k: d[k] for k in ("id", "frames", "first_t", "a", "b")} for d in per if d["frames"]]
    lengths = [{"id": d["id"], "len": d["len"]} for d in per if d["len"][0] != d["len"][1]]
    return {"cases": cases, "rows": sum(d["rows"] for d in per), "differing_cases": len(diffs),
            "differing_rows": sum(d["frames"] for d in diffs), "max_cents_gap": max((d["gap"] for d in per), default=0.0),
            "length_mismatches": lengths[:20], "diffs": diffs[:50]}


def key(f):
    """What must match between platforms: validity, the live note, and the number on screen. The
    note is compared only while live because the phone's log names the reading's note, not the
    held one (the number during the hold is still compared)."""
    return (f["valid"], f["midi"] if f["valid"] else 0, f["shown"])


def row_text(f):
    return f"{f['t']:.3f}s midi {f['midi']} valid {int(f['valid'])} shown {f['shown']}"


def like_for_like(fa, fb, a, b, cases):
    """Synthetic and regression audio is made on each machine with its own libm, so two machines'
    runs of it do not share an input: it is compared only through the phone pack's WAVs (--extra)."""
    if a.split("-")[0] == b.split("-")[0]:
        return fa, fb
    real = {k for k in set(fa) & set(fb) if cases.get(k, {}).get("dataset") not in ("synthetic", "regression")}
    return {k: fa[k] for k in real}, {k: fb[k] for k in real}


def all_pairs(all_frames, cases):
    """Every pair of platforms on a shared frame grid, like for like."""
    out, names = [], sorted(all_frames)
    for i, a in enumerate(names):
        for b in names[i + 1:]:
            c = compare(*like_for_like(all_frames[a], all_frames[b], a, b, cases))
            if "grid" not in c:
                out.append({"a": a, "b": b, **c})
    return out


def compare_logs(args):
    """Two platforms' per-file logs of the same WAVs: {id}{a-glob} against {id}{b-glob}."""
    a, b = {}, {}
    for name in os.listdir(args.a):
        if name.endswith(args.a_glob):
            cid = name[:-len(args.a_glob)].removeprefix("play-")
            other = os.path.join(args.b, args.b_prefix + cid + args.b_glob)
            if os.path.exists(other):
                a[cid] = M.load_frames(os.path.join(args.a, name), case_col=False)[""]
                b[cid] = M.load_frames(other, case_col=False)[""]
    a_name, b_name = args.name.split(" vs ")
    out = {"a": a_name, "b": b_name, **compare(a, b)}
    with open(args.out, "w") as f:
        json.dump(out, f, indent=1)
    print(json.dumps({k: v for k, v in out.items() if k != "diffs"}))


def platform_frames(paths):
    out = {}
    for p in paths:
        out.update(M.load_frames(p))
    return out


def collect(args):
    """Per-file device logs into frames-<platform>-b64-<dataset>.csv, the bulk format."""
    os.makedirs(args.out, exist_ok=True)
    outs = {}
    for name in sorted(os.listdir(args.logs)):
        if not (name.startswith("play-") and name.endswith(".csv")):
            continue
        cid = name[len("play-"):-len(".csv")]
        case_path = os.path.join(args.pack, cid + ".case.json")
        if not os.path.exists(case_path):
            continue
        with open(case_path) as f:
            dataset = json.load(f)["dataset"]
        with open(os.path.join(args.logs, name)) as f:
            rows = f.read().splitlines()[1:]
        out = outs.setdefault(dataset, [])
        out += [f"{cid},{r}" for r in rows if r.strip()]
    for dataset, rows in outs.items():
        with open(os.path.join(args.out, f"frames-{args.platform}-b64-{dataset}.csv"), "w") as f:
            f.write("case," + M_HEADER + "\n" + "\n".join(rows) + "\n")
    print(f"{sum(len(v) for v in outs.values())} rows from {args.logs}")


M_HEADER = "t_ms,note,octave,cents,hz,valid,shown_cents"


def diff_files(args):
    a = M.load_frames(args.a, case_col=False)
    b = M.load_frames(args.b, case_col=False)
    r = compare(a, b)
    print(json.dumps(r, indent=1))
    return 1 if r["differing_cases"] else 0


