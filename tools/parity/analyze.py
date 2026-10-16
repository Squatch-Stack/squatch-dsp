#!/usr/bin/env python3
"""Compare the two renders of every case: python3 -I analyze.py OUT_DIR  ->  OUT_DIR/results.json + results.md.

For each case, with JS (the browser kernel) as the reference and C++ (the squatch-dsp block) as the
candidate, after aligning them by cross-correlation:
  lag        samples the C++ output trails the browser's (cross-correlation peak, +-4096), and the
             latency each side declares
  level      RMS(C++) / RMS(JS) in dB; peak likewise
  esr        sum (c - j)^2 / sum j^2
  esr_gain   the same after the best gain on c (a pure level difference costs nothing here)
  third-octave magnitude difference C++ - JS in dB, over the ISO bands 63 Hz .. 16 kHz that hold
             energy within 70 dB of the loudest band: median |d|, max |d| and where, median signed d
  gain curve (gate, comp, limiter) 10 ms-window gain = RMS(out)/RMS(in) in dB on each side; the
             difference C++ - JS: median |d|, 95th percentile |d|, max |d|
  true peak  (limiter) 8x-oversampled peak of each output
"""
import json
import sys
from pathlib import Path

import numpy as np
from scipy import signal

SR = 48000


def rd(p):
    return np.fromfile(p, dtype="<f4").astype(np.float64)


def db(x):
    return 20 * np.log10(max(x, 1e-12))


def rms(x):
    return float(np.sqrt(np.mean(x * x))) if len(x) else 0.0


def lag_of(j, c, span=4096):
    n = min(len(j), len(c), 20 * SR)
    a, b = j[:n], c[:n]
    # exact search for |lag| <= span via FFT cross-correlation
    f = 1 << (2 * n - 1).bit_length()
    xc = np.fft.irfft(np.fft.rfft(b, f) * np.conj(np.fft.rfft(a, f)), f)
    cand = np.concatenate([xc[:span + 1], xc[-span:]])
    idx = np.concatenate([np.arange(span + 1), np.arange(-span, 0)])
    return int(idx[np.argmax(cand)])


def third_octave(x, bands):
    f, p = signal.welch(x, SR, nperseg=16384, noverlap=8192, window="hann")
    res = []
    for fc in bands:
        lo, hi = fc / 2 ** (1 / 6), fc * 2 ** (1 / 6)
        m = (f >= lo) & (f < hi)
        res.append(float(p[m].sum()) if m.any() else 0.0)
    return np.array(res)


BANDS = [1000 * 2 ** (k / 3) for k in range(-13, 13) if 60 <= 1000 * 2 ** (k / 3) <= 16500]


def spectrum_diff(j, c):
    pj, pc = third_octave(j, BANDS), third_octave(c, BANDS)
    top = pj.max()
    keep = [i for i, v in enumerate(pj) if v > top * 1e-7 and pc[i] > 0]
    d = np.array([10 * np.log10(pc[i] / pj[i]) for i in keep])
    fk = [BANDS[i] for i in keep]
    k = int(np.argmax(np.abs(d)))
    return dict(median_abs=float(np.median(np.abs(d))), max_abs=float(np.abs(d)[k]), max_at_hz=round(fk[k]),
                median=float(np.median(d)), bands=len(d))


def window_gain(y, x, win=480):
    n = (min(len(y), len(x)) // win) * win
    ry = np.sqrt((y[:n].reshape(-1, win) ** 2).mean(1))
    rx = np.sqrt((x[:n].reshape(-1, win) ** 2).mean(1))
    ok = rx > 10 ** (-80 / 20)
    return np.where(ok, 20 * np.log10(np.maximum(ry, 1e-12) / np.maximum(rx, 1e-12)), np.nan)


def true_peak(x):
    return db(np.abs(signal.resample_poly(x, 8, 1)).max())


def rt60_t20(y, start):
    """T20-extrapolated decay time of the wet impulse response y[start:], from its Schroeder curve."""
    e = np.cumsum((y[start:] ** 2)[::-1])[::-1]
    e = 10 * np.log10(e / e[0] + 1e-30)
    t = np.arange(len(e)) / SR
    m = (e <= -5) & (e >= -25)
    if m.sum() < 10:
        return float("nan")
    s = np.polyfit(t[m], e[m], 1)[0]
    return float(-60 / s)


def aligned(j, k, lag):
    n = min(len(j), len(k))
    if lag > 0:
        return j[:n - lag], k[lag:n]
    if lag < 0:
        return j[-lag:n], k[:n + lag]
    return j[:n], k[:n]


def refine(j, k, lag):
    """Of lag-3..lag+3 the one with the lowest ESR after the best gain (a filtered, clipped signal has a smeared correlation peak)."""
    best, bl = None, lag
    for l in range(lag - 3, lag + 4):
        a, b = aligned(j, k, l)
        g = np.dot(b, a) / max(np.dot(b, b), 1e-30)
        e = np.sum((g * b - a) ** 2) / max(np.sum(a * a), 1e-30)
        if best is None or e < best:
            best, bl = e, l
    return bl


def harmonics(y, f0, n=7):
    """Levels of harmonics 1..n relative to the fundamental, and the non-harmonic power (dB re fundamental)."""
    seg = y[SR // 4: SR // 4 + 32768]  # skip the start-up, a whole number of cycles is not needed with a flat-top window
    w = signal.get_window("flattop", len(seg))
    sp = np.abs(np.fft.rfft(seg * w)) ** 2
    hz = np.fft.rfftfreq(len(seg), 1 / SR)
    def band(fc, hw=60):
        return sp[(hz > fc - hw) & (hz < fc + hw)].sum()
    p1 = band(f0)
    hs = [10 * np.log10(max(band(f0 * m), 1e-30) / p1) for m in range(2, n + 1)]
    known = np.zeros(len(sp), bool)
    for m in range(1, n + 1):
        known |= (hz > f0 * m - 60) & (hz < f0 * m + 60)
    nonh = 10 * np.log10(max(sp[~known & (hz > 100)].sum(), 1e-30) / p1)
    return hs, nonh


def base_metrics(c, j, k, lag, lat):
    """Level, peak, ESR (raw and after the best gain) and third-octave difference of the aligned pair."""
    j2, k2 = aligned(j, k, lag)
    r = dict(id=c["id"], pair=c["pair"], note=c["note"], lag=lag, js_latency=lat[0], cpp_latency=lat[1],
             rms_js_db=db(rms(j2)), rms_cpp_db=db(rms(k2)), level_db=db(rms(k2)) - db(rms(j2)),
             peak_js_db=db(np.abs(j2).max()), peak_cpp_db=db(np.abs(k2).max()))
    r["peak_db"] = r["peak_cpp_db"] - r["peak_js_db"]
    den = float(np.sum(j2 ** 2))
    r["esr"] = float(np.sum((k2 - j2) ** 2)) / den if den else float("nan")
    g = float(np.dot(k2, j2) / max(np.dot(k2, k2), 1e-30))
    r["best_gain_db"] = db(abs(g))
    r["esr_gain"] = float(np.sum((g * k2 - j2) ** 2) / den) if den else float("nan")
    r["spec"] = spectrum_diff(j2, k2)
    return r


def gain_curve(x, j, k, lat):
    """C++ minus browser 10 ms-window gain, each output against the input delayed by that side's own latency."""
    x = np.concatenate([x, np.zeros(len(j))])[: len(j)]

    def one(y, delay):
        return window_gain(y, np.concatenate([np.zeros(delay), x])[: len(y)])

    d = np.abs(one(k, lat[1]) - one(j, lat[0]))
    d = d[~np.isnan(d)]
    return dict(median_abs=float(np.median(d)), p95_abs=float(np.percentile(d, 95)), max_abs=float(d.max()),
                windows=len(d), frac_over_3db=float(np.mean(d > 3.0)))


def reverb_metrics(jr, j, k, lag):
    """The C++ mono output against the browser's mid, and how decorrelated the browser's two channels are."""
    _, jm = aligned(j, 0.5 * (j + jr), lag)
    _, k2 = aligned(j, k, lag)
    return dict(esr_vs_js_mid=float(np.sum((k2 - jm) ** 2) / np.sum(jm ** 2)), lr_corr_js=float(np.corrcoef(j, jr)[0, 1]),
                js_right_vs_left_db=db(rms(jr)) - db(rms(j)))


def extra_metrics(c, out, j, k):
    """Pair-specific measurements: true peak, harmonics, reverb stereo and decay."""
    r = {}
    if c["pair"] == "limiter":
        r["true_peak_js_db"], r["true_peak_cpp_db"] = true_peak(j), true_peak(k)
    if c.get("harm"):
        hj, nj = harmonics(j, c["harm"])
        hk, nk = harmonics(k, c["harm"])
        r["harm_js"], r["harm_cpp"], r["nonharm_js_db"], r["nonharm_cpp_db"] = hj, hk, nj, nk
    if c["id"] == "reverb-impulse":
        r["rt60_js"], r["rt60_cpp"] = rt60_t20(j, int(0.2 * SR)), rt60_t20(k, int(0.2 * SR))
    return r


def measure(c, out, lat):
    cid = c["id"]
    j, k = rd(out / "js" / f"{cid}.f32"), rd(out / "cpp" / f"{cid}.f32")
    n = min(len(j), len(k))
    j, k = j[:n], k[:n]
    lag = refine(j, k, lag_of(j, k)) if np.abs(j).max() > 0 else 0
    if cid == "limiter-isp":  # a steady 12 kHz tone has a correlation peak every 4 samples: use the declared latencies
        lag = lat[1] - lat[0]
    r = base_metrics(c, j, k, lag, lat)
    if c["pair"] in ("gate", "comp", "limiter"):
        x = rd(out / "in" / f"{c['input']}.f32") * 10 ** (c["gainDb"] / 20)
        r["gain_curve"] = gain_curve(x, j, k, lat)
    if c["pair"] == "reverb":
        r.update(reverb_metrics(rd(out / "js" / f"{cid}.R.f32")[:n], j, k, lag))
    j2, k2 = aligned(j, k, lag)
    r.update(extra_metrics(c, out, j2 if c.get("harm") else j, k2 if c.get("harm") else k))
    return r


def fmt(v, p=2):
    return ("%.*f" % (p, v)) if v == v else "n/a"


ROW_HEAD = ("| case | lag (js/cpp latency) | level dB | peak dB | ESR | ESR after gain | 1/3-oct med / max dB (at Hz) "
            "| gain curve: med / p95 / max dB, share of 10 ms windows >3 dB apart |")


def row(r):
    gcv = r.get("gain_curve")
    curve = (f"{fmt(gcv['median_abs'])} / {fmt(gcv['p95_abs'])} / {fmt(gcv['max_abs'])}, {100 * gcv['frac_over_3db']:.1f} %"
             if gcv else "-")
    sp = r["spec"]
    return (f"| {r['id']} | {r['lag']} ({r['js_latency']}/{r['cpp_latency']}) | {fmt(r['level_db'])} | {fmt(r['peak_db'])} "
            f"| {fmt(r['esr'], 4)} | {fmt(r['esr_gain'], 4)} | {fmt(sp['median_abs'])} / {fmt(sp['max_abs'])} ({sp['max_at_hz']}) | {curve} |")


def notes(r):
    extra = []
    if "true_peak_js_db" in r:
        extra.append(f"true peak js {fmt(r['true_peak_js_db'])} / cpp {fmt(r['true_peak_cpp_db'])} dBTP; "
                     f"sample peak js {fmt(r['peak_js_db'])} / cpp {fmt(r['peak_cpp_db'])} dBFS")
    if "rt60_js" in r:
        extra.append(f"T20-extrapolated RT60 js {fmt(r['rt60_js'])} s / cpp {fmt(r['rt60_cpp'])} s")
    if "js_right_vs_left_db" in r:
        extra.append(f"js right vs left {fmt(r['js_right_vs_left_db'])} dB, L/R correlation {fmt(r['lr_corr_js'])}, "
                     f"C++ mono vs js (L+R)/2 ESR {fmt(r['esr_vs_js_mid'], 4)}")
    if "harm_js" in r:
        extra.append("H2..H7 re fundamental, dB: js " + " ".join(fmt(v, 1) for v in r["harm_js"]) + " | cpp "
                     + " ".join(fmt(v, 1) for v in r["harm_cpp"])
                     + f"; non-harmonic (aliasing) js {fmt(r['nonharm_js_db'], 1)} / cpp {fmt(r['nonharm_cpp_db'], 1)} dB")
    return f"- {r['id']}: " + "; ".join(extra) if extra else None


def markdown(results):
    lines = []
    for pair in dict.fromkeys(r["pair"] for r in results):
        rs = [x for x in results if x["pair"] == pair]
        lines += [f"### {pair}", "", ROW_HEAD, "|---|---|---|---|---|---|---|---|"] + [row(r) for r in rs] + [""]
        lines += [n for n in (notes(r) for r in rs) if n] + [""]
    return "\n".join(lines)


def main(out):
    cases = json.loads((out / "cases.json").read_text())
    cpp_lat = json.loads((out / "cpp_latency.json").read_text())
    js_lat = {}
    for line in (out / "js.log").read_text().splitlines():
        d = json.loads(line)
        js_lat[d["id"]] = d["latency"]
    results = [measure(c, out, (js_lat[c["id"]], cpp_lat[c["id"]])) for c in cases if c["pair"] != "odfit"]
    (out / "results.json").write_text(json.dumps(results, indent=1))
    text = markdown(results)
    (out / "results.md").write_text(text)
    print(text)


if __name__ == "__main__":
    main(Path(sys.argv[1]))
