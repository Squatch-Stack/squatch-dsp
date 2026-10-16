# SPDX-License-Identifier: MIT
"""index.html for the scorecard: verdicts, bar charts against the thresholds, display traces and
the tables, as one self-contained page (inline SVG and script, Squatch Ink colours, light and dark)."""
import json

import breakdowns_md
import report as R

CSS = """
:root{color-scheme:light;--page:#F7F5F0;--raised:#FCFBF8;--sunken:#E8E0D4;--ink:#1C160E;--ink2:#6B5B3E;--rule:#D4CBBB;
--s1:#2a78d6;--s2:#eb6834;--limit:#9A2E1B;--good:#2f6b2f}
@media (prefers-color-scheme:dark){:root:not([data-theme="light"]){color-scheme:dark;--page:#17130D;--raised:#201A12;
--sunken:#231D15;--ink:#EDE6D8;--ink2:#A89878;--rule:#3A3226;--s1:#3987e5;--s2:#d95926;--limit:#D9553D;--good:#7fbf7f}}
:root[data-theme="dark"]{color-scheme:dark;--page:#17130D;--raised:#201A12;--sunken:#231D15;--ink:#EDE6D8;--ink2:#A89878;
--rule:#3A3226;--s1:#3987e5;--s2:#d95926;--limit:#D9553D;--good:#7fbf7f}
body{margin:0;background:var(--page);color:var(--ink);font:14px/1.5 Inter,ui-sans-serif,-apple-system,"Segoe UI",sans-serif}
main{max-width:1100px;margin:0 auto;padding:16px}
h1{font:italic 500 28px/1.2 Lora,Georgia,serif;margin:8px 0}h2{font:italic 500 20px/1.2 Lora,Georgia,serif;margin:28px 0 8px}
a{color:var(--s1)}.muted{color:var(--ink2)}
.tiles{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:8px}
.tile{background:var(--raised);border:1px solid var(--rule);border-radius:10px;padding:10px}
.tile b{display:block;font:600 11px/1 Inter,sans-serif;letter-spacing:1px;text-transform:uppercase;color:var(--ink2)}
.pass{color:var(--good);font-weight:600}.fail{color:var(--limit);font-weight:600}.pend{color:var(--ink2)}
.scroll{overflow-x:auto;border:1px solid var(--rule);border-radius:10px;background:var(--raised)}
table{border-collapse:collapse;width:100%;font:12px/1.3 "IBM Plex Mono",ui-monospace,Menlo,monospace}
th,td{padding:5px 8px;border-bottom:1px solid var(--rule);text-align:right;white-space:nowrap}th{color:var(--ink2);font-weight:600}
td:first-child,th:first-child,td.l{text-align:left}
svg{display:block;width:100%;height:auto}svg text{fill:var(--ink2);font:11px Inter,sans-serif}
.grid{stroke:var(--rule);stroke-width:1}.limit{stroke:var(--limit);stroke-width:1.5;stroke-dasharray:4 3}
#tip{position:fixed;pointer-events:none;background:var(--raised);border:1px solid var(--rule);border-radius:6px;padding:6px 8px;
font:12px "IBM Plex Mono",monospace;display:none;z-index:9}
.legend span{display:inline-flex;align-items:center;gap:6px;margin-right:14px}.legend i{width:12px;height:3px;border-radius:2px;display:inline-block}
"""

SCRIPT = """
const tip=document.getElementById('tip');
document.querySelectorAll('[data-tip]').forEach(el=>{el.addEventListener('mousemove',e=>{tip.style.display='block';
tip.textContent=el.dataset.tip;tip.style.left=(e.clientX+12)+'px';tip.style.top=(e.clientY+12)+'px'});
el.addEventListener('mouseleave',()=>tip.style.display='none')});
document.querySelectorAll('svg[data-trace]').forEach(svg=>{const d=JSON.parse(svg.dataset.trace),x=svg.querySelector('.cross');
svg.addEventListener('mousemove',e=>{const r=svg.getBoundingClientRect(),vx=(e.clientX-r.left)/r.width*d.w;
if(vx<d.l||vx>d.w-d.r){x.style.display='none';tip.style.display='none';return}
const t=d.t0+(vx-d.l)/(d.w-d.l-d.r)*(d.t1-d.t0);let i=0;while(i+1<d.t.length&&Math.abs(d.t[i+1]-t)<Math.abs(d.t[i]-t))i++;
x.setAttribute('x1',vx);x.setAttribute('x2',vx);x.style.display='block';tip.style.display='block';
tip.textContent=`t ${d.t[i].toFixed(2)} s · shown ${d.s[i]==null?'--.-':d.s[i].toFixed(1)} c · truth ${d.u[i]==null?'–':d.u[i].toFixed(2)} c`;
tip.style.left=(e.clientX+12)+'px';tip.style.top=(e.clientY+12)+'px'});
svg.addEventListener('mouseleave',()=>{x.style.display='none';tip.style.display='none'})});
const h=document.getElementById('home'),p=location.hostname.split('.');
if(p.length>=3&&!/^\\d+$/.test(p[p.length-1]))h.href='https://home.'+p.slice(1).join('.')+'/';
"""


def bars(title, rows, limit, unit):
    """Horizontal bars, one row per (label, [(series, value)]), with the pass threshold dashed."""
    w, left, right, bh, gap = 900, 190, 30, 12, 2
    series_n = max(len(v) for _, v in rows) if rows else 1
    rowh = series_n * (bh + gap) + 12
    h = 28 + rowh * len(rows) + 22
    vmax = max([limit * 1.5] + [v for _, vs in rows for _, v in vs if v is not None])
    xs = lambda v: left + (w - left - right) * min(v, vmax) / vmax  # noqa: E731
    out = [f'<svg viewBox="0 0 {w} {h}" role="img" aria-label="{R.esc(title)}">']
    for k in range(5):
        v = vmax * k / 4
        out.append(f'<line class="grid" x1="{xs(v):.1f}" x2="{xs(v):.1f}" y1="20" y2="{h - 20}"/><text x="{xs(v):.1f}" y="{h - 6}" text-anchor="middle">{v:.2g}{unit}</text>')
    for i, (label, vs) in enumerate(rows):
        y = 28 + i * rowh
        out.append(f'<text x="{left - 8}" y="{y + rowh / 2 - 2:.1f}" text-anchor="end">{R.esc(label)}</text>')
        for j, (name, v) in enumerate(vs):
            if v is None:
                continue
            yy = y + j * (bh + gap)
            out.append(f'<rect x="{left}" y="{yy}" width="{max(2, xs(v) - left):.1f}" height="{bh}" rx="3" fill="var(--s{j + 1})" '
                       f'data-tip="{R.esc(label)} · {R.esc(name)}: {v:.3f}{unit}"/>')
            out.append(f'<text x="{xs(v) + 4:.1f}" y="{yy + bh - 2}">{v:.2f}</text>')
    out.append(f'<line class="limit" x1="{xs(limit):.1f}" x2="{xs(limit):.1f}" y1="16" y2="{h - 20}"/>'
               f'<text x="{xs(limit) + 4:.1f}" y="14" style="fill:var(--limit)">pass ≤ {limit}{unit}</text></svg>')
    return "".join(out)


def trace(tr, title):
    """The shown number and the truth over time for one case (cents against the shown note)."""
    w, h, l, r, top, bot = 900, 260, 46, 16, 12, 28
    t = tr["t"]
    vals = [v for v in tr["shown"] + tr["truth"] if v is not None]
    if not t or not vals:
        return ""
    lo, hi = min(vals) - 0.5, max(vals) + 0.5
    X = lambda x: l + (w - l - r) * (x - t[0]) / max(1e-9, t[-1] - t[0])  # noqa: E731
    Y = lambda y: top + (h - top - bot) * (hi - y) / (hi - lo)  # noqa: E731
    out = [f'<svg viewBox="0 0 {w} {h}" role="img" aria-label="{R.esc(title)}" data-trace=\'{json.dumps({"t": t, "s": tr["shown"], "u": tr["truth"], "w": w, "l": l, "r": r, "t0": t[0], "t1": t[-1]})}\'>']
    for k in range(6):
        y = lo + (hi - lo) * k / 5
        out.append(f'<line class="grid" x1="{l}" x2="{w - r}" y1="{Y(y):.1f}" y2="{Y(y):.1f}"/><text x="{l - 6}" y="{Y(y) + 4:.1f}" text-anchor="end">{y:.1f}</text>')
    for k in range(9):
        x = t[0] + (t[-1] - t[0]) * k / 8
        out.append(f'<text x="{X(x):.1f}" y="{h - 8}" text-anchor="middle">{x:.1f} s</text>')
    for key, n in (("truth", 2), ("shown", 1)):
        out.append(f'<path d="{path(t, tr[key], X, Y)}" fill="none" stroke="var(--s{n})" stroke-width="2" stroke-linejoin="round"/>')
    out.append(f'<line class="cross" x1="0" x2="0" y1="{top}" y2="{h - bot}" stroke="var(--ink2)" style="display:none"/></svg>')
    return "".join(out)


def path(t, v, X, Y):
    d, pen = [], False
    for x, y in zip(t, v):
        if y is None:
            pen = False
            continue
        d.append(f'{"L" if pen else "M"}{X(x):.1f},{Y(y):.1f}')
        pen = True
    return "".join(d)


def md_table_html(lines):
    """A Markdown table (from report.py) as HTML."""
    rows = [ln.strip("|").split("|") for ln in lines if ln.startswith("|") and not ln.startswith("|---")]
    if not rows:
        return ""
    head = "".join(f"<th>{R.esc(c.strip())}</th>" for c in rows[0])
    body = "".join("<tr>" + "".join(cell(c) for c in r) + "</tr>" for r in rows[1:])
    return f'<div class="scroll"><table><thead><tr>{head}</tr></thead><tbody>{body}</tbody></table></div>'


def cell(c):
    c = c.strip()
    cls = "fail" if "FAIL" in c else "pass" if "✓" in c or c == "PASS" else "pend" if c == "pending" else ""
    return f'<td class="{cls}">{R.esc(c)}</td>'


def verdict_tiles(board):
    out = ['<div class="tiles">']
    for p in R.platforms_present(board) + R.pending_platforms(board):
        n = len(board["platforms"].get(p, {}).get("results", []))
        if n:
            s, a = R.verdict(board, p)
            st = f'<span class="{"pass" if s else "fail"}">{"PASS" if s else "FAIL"}</span>'
            ac = f'<span class="{"pass" if a else "fail"}">{"PASS" if a else "FAIL"}</span>'
        else:
            st = ac = '<span class="pend">pending</span>'
        scope = f'<br><span class="muted">{n} cases{" (partial)" if 0 < n < 100 else ""}</span>' if n else ""
        out.append(f'<div class="tile"><b>{R.esc(p)}</b>steadiness {st}<br>accuracy {ac}{scope}</div>')
    return "".join(out) + "</div>"


def notes_html(notes):
    """Paragraphs; a paragraph whose lines start with "- " becomes a lead line and a list."""
    out = []
    for n in notes:
        lines = n.split("\n")
        items = [ln[2:] for ln in lines if ln.startswith("- ")]
        lead = " ".join(ln for ln in lines if not ln.startswith("- "))
        out.append(f'<p>{R.esc(lead)}</p>' + (f'<ul>{"".join(f"<li>{R.esc(i)}</li>" for i in items)}</ul>' if items else ""))
    return "".join(out)


def dataset_bars(board, key, limit, unit, title):
    shown = [p for p in R.platforms_present(board) if p.endswith("b16")][:2]
    rows = []
    for d, t in R.DATASET_ROWS:
        vals = [(p, (R.group(board, p, "dataset", d, t) or {}).get(key)) for p in shown]
        if any(v is not None for _, v in vals):
            rows.append((f"{d} ({t})", vals))
    legend = "".join(f'<span><i style="background:var(--s{i + 1})"></i>{R.esc(p)}</span>' for i, p in enumerate(shown))
    return f'<h2>{R.esc(title)}</h2><div class="legend muted">{legend}</div>' + bars(title, rows, limit, unit)


def before_after_html(board):
    """The before -> after verdicts and the first platform's per-dataset measures, when there is a baseline."""
    shown = R.both(board)
    if not shown:
        return ""
    return ("<h2>Before and after</h2>" + md_table_html(R.md_before_after_verdict(board)) +
            f"<h2>Before → after ({R.esc(shown[0])})</h2>" + md_table_html(R.md_before_after(board, shown[0])))


def write(board, path_out, notes):
    p0 = R.platforms_present(board)[0]
    parts = [f"<!doctype html><html lang=en><head><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
             f"<title>Tuner quality scorecard</title><style>{CSS}</style></head><body><main>",
             '<p><a id="home" href="../">← Squatch home</a></p><h1>Squatch Tuner quality scorecard</h1>',
             notes_html(notes), verdict_tiles(board), breakdowns_md.html_top(board), before_after_html(board),
             dataset_bars(board, "wobble_p95", 0.5, " c", "Steadiness: wobble of the shown number over 300 ms (p95)"),
             '<p class="muted">Peak-to-peak of the number on screen about its own straight line, over every 300 ms from 0.5 s into a '
             'note. On real notes this includes the string\'s own waver; the synthetic verdict uses what the tuner adds to the exact truth.</p>',
             dataset_bars(board, "err_p95", 1.0, " c", "Accuracy: absolute error against the truth, steady state (p95)")]
    for cid, tr in board.get("traces", {}).items():
        parts.append(f'<h2>Trace: {R.esc(cid)}</h2><p class="muted">{R.esc(tr["why"])}</p><div class="legend muted"><span><i style="background:var(--s1)">'
                     f'</i>shown number (c)</span><span><i style="background:var(--s2)"></i>truth (c)</span></div>' + trace(tr, cid))
    for title, lines in (("Per dataset", R.md_dataset_table(board, p0)), ("Owner takes", R.md_owner(board, p0)),
                         ("Synthetic conditions", R.md_conditions(board, p0)), ("Platforms compared frame by frame", R.md_comparisons(board))):
        parts.append(f"<h2>{R.esc(title)} ({R.esc(p0)})</h2>" + md_table_html(lines))
    parts.append(f"<h2>Worst cases ({R.esc(p0)})</h2>" + "".join(md_table_html(ch.split("\n")) for ch in "\n".join(R.md_worst(board, p0)).split("\n\n")))
    parts.append(breakdowns_md.html_sections(board))
    parts.append(f'<div id="tip"></div><script>{SCRIPT}</script></main></body></html>')
    with open(path_out, "w") as f:
        f.write("".join(parts))
