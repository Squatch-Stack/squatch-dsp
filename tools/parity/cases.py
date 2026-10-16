#!/usr/bin/env python3
"""Build the parity inputs and the case list: python3 -I cases.py RIG_DIR OUT_DIR.

Inputs are raw float32 mono at 48 kHz in OUT_DIR/in/. They come from squatch-rig's credited, public
DI loops (web/audio/loops, "Five guitar dataset", CC BY 4.0) plus two synthetic ones. Nothing under
squatch-rig/private or any private capture is read.

A case is one pair of renders at matched settings:
  js   the browser kernel and its parameters (names are the kernel's own)
  cpp  the squatch-dsp block's type id and its contract keys
  note what the case is for
"mapped" cases apply the parameter mapping from docs/PARITY-2026-10.md; "default" cases use the
browser defaults on both sides; "own-defaults" leaves each side at its own shipped default.
"""
import json
import math
import sys
from collections import namedtuple
from pathlib import Path

import numpy as np
import scipy.io.wavfile as wavfile

rig, out = Path(sys.argv[1]), Path(sys.argv[2])
(out / "in").mkdir(parents=True, exist_ok=True)
SR = 48000


def loop(name):
    r, d = wavfile.read(rig / "web/audio/loops" / f"{name}.wav")
    assert r == SR, (name, r)
    scale = {np.dtype("int16"): 32768.0, np.dtype("int32"): 2147483648.0}.get(d.dtype, 1.0)
    d = np.asarray(d, dtype=np.float64) / scale
    assert np.abs(d).max() <= 1.0, (name, "not full-scale-normalised")
    return d


def save(name, x):
    np.asarray(x, dtype="<f4").tofile(out / "in" / f"{name}.f32")


chords, stabs, riff = loop("clean-chords"), loop("chord-stabs"), loop("picked-riff")
save("chords", chords)
save("stabs", stabs)
save("riff", riff)
# The clean chords with a steady hiss under them (white, rms -75 dBFS, fixed seed): a gate has work.
rng = np.random.default_rng(20261011)
save("chords_hiss", chords + rng.standard_normal(len(chords)) * 10 ** (-75 / 20))
# The chords again, each chord let ring then fade 60 dB and a gap of hiss only (1.5 s cycle): the gate
# has to open, hold, close and reopen. Hiss is added after the envelope, so it never fades.
t = np.arange(len(chords)) / SR % 1.5
env = np.where(t < 0.4, 1.0, np.where(t < 1.0, 10 ** (-60 * (t - 0.4) / 0.6 / 20), 0.0))
save("chords_gated", chords * env + rng.standard_normal(len(chords)) * 10 ** (-75 / 20))
# A 3.1 kHz sine at -12 dBFS, 1 s (harmonics 2..7 stay under Nyquist; everything else is aliasing), and a
# 12 kHz (fs/4) sine at 45 degrees, -0.9 dBFS: its samples peak at -4 dBFS, its waveform at -0.9 (a true-peak test).
n1 = np.arange(SR)
save("sine3100", 10 ** (-12 / 20) * np.sin(2 * np.pi * 3100 * n1 / SR))
fade = np.minimum(1.0, np.minimum(n1, n1[::-1]) / (0.1 * SR))  # 100 ms fades, so no onset transient adds its own overshoot
save("fs4", fade * 10 ** (-0.9 / 20) * np.sin(2 * np.pi * 12000 * n1 / SR + np.pi / 4))
save("riff6", riff[: 6 * SR])
# A single click, then silence: the reverb's impulse response (6 s).
imp = np.zeros(6 * SR)
imp[SR // 10] = 1.0
save("impulse", imp)

CASES = []


Spec = namedtuple("Spec", "pair kind typeid")  # which browser kernel is paired with which squatch-dsp block


def case(cid, spec, js, cpp, **opt):
    """One pair of renders. opt: input (required), gain (dB), tail (s), note, harm (Hz), jsx / cppx (extra fields)."""
    CASES.append(dict(id=cid, pair=spec.pair, input=opt["input"], gainDb=opt.get("gain", 0.0), tail=opt.get("tail", 0.0),
                      note=opt.get("note", ""), harm=opt.get("harm"),
                      js=dict(kind=spec.kind, params=js, **opt.get("jsx", {})),
                      cpp=dict(type=spec.typeid, params=cpp, **opt.get("cppx", {}))))


# ---- gate: browser NoiseGate  <->  cc.squatch.gate ------------------------------------------------
G = "cc.squatch.gate"
gd = dict(threshold=-58, hysteresis=6, attack=1, hold=40, release=90, range=-70)  # browser defaults
case("gate-default", Spec("gate", "gate", G), gd, gd, input="chords_gated", note="browser defaults on both sides")
case("gate-own-defaults", Spec("gate", "gate", G), {}, {}, input="chords_gated", note="each side at its shipped default (browser -58/6/1/40/90/-70, C++ -60/6/1/100/330/-60)")
ga = dict(threshold=-45, hysteresis=6, attack=1, hold=40, release=90, range=-70)
case("gate-A", Spec("gate", "gate", G), ga, ga, input="chords_gated", gain=-20, note="threshold -45 inside the chord decays")
gb = dict(threshold=-35, hysteresis=3, attack=5, hold=100, release=300, range=-30)
case("gate-B", Spec("gate", "gate", G), gb, gb, input="chords_gated", gain=-20, note="slow, shallow: range -30 dB")
gc_ = dict(threshold=-50, hysteresis=0, attack=0.5, hold=0, release=50, range=-60)
case("gate-C", Spec("gate", "gate", G), gc_, gc_, input="chords_gated", gain=-20, note="no hold, no hysteresis: the detectors' own behaviour shows")



def gate_map(p):
    """C++ gate parameters that match the browser's. The browser's attack/release are one-pole time
    constants (63 % of the way in that time; about 8.7 dB per time constant at first); the C++
    ones are the time to cross the whole range at a constant dB/s. Equal initial slope:
    release_cpp = tau * range_dB / 8.686. Opening is ~instant either way: attack_cpp = 1.2 tau
    (the browser is within 3 dB of open after 1.2 tau)."""
    q = dict(p)
    rng = max(-p["range"], 6.0)
    q["release"] = min(1000.0, max(5.0, p["release"] * rng / 8.686))
    q["attack"] = min(500.0, max(0.1, 1.2 * p["attack"]))
    return {k: round(v, 3) for k, v in q.items()}


for cid in ("gate-default", "gate-A", "gate-B", "gate-C"):
    c0 = next(x for x in CASES if x["id"] == cid)
    case(cid + "-mapped", Spec("gate", "gate", G), c0["js"]["params"], gate_map(c0["js"]["params"]), input=c0["input"], gain=c0["gainDb"], note="C++ attack/release converted from time constants to range-crossing times (gate_map)")

# ---- compressor: browser Compressor  <->  cc.squatch.comp (a port of the same kernel) -----------
C = "cc.squatch.comp"
cd = dict(threshold=-24, ratio=4, attack=5, release=120, knee=6, makeup=6, mix=1)
case("comp-default", Spec("comp", "compressor", C), cd, cd, input="stabs", note="both sides' defaults are identical")
ca = dict(threshold=-30, ratio=10, attack=1, release=50, knee=0, makeup=0, mix=1)
case("comp-A", Spec("comp", "compressor", C), ca, ca, input="stabs", note="hard knee, fast")
cb = dict(threshold=-18, ratio=2, attack=20, release=400, knee=12, makeup=3, mix=0.5)
case("comp-B", Spec("comp", "compressor", C), cb, cb, input="stabs", note="soft knee, slow, 50 % blend (the 20 ms mix smoother is C++ only)")

# ---- Trail OD: browser Overdrive  <->  cc.squatch.trail-od -----------------------------------------
T = "cc.squatch.trail-od"


def od_map(drive, tone):
    """C++ (drive, tone) whose clipper-branch gain and tone corner equal the browser's.

    browser: gain = 1 + 110 d^2; corner = 500 * 2^(4.5 t) Hz.   C++: gain = (51k + 500k d) / 4.7k;
    corner = 300 + 6000 t^2 Hz.  Solved for the C++ knobs; clamped to 0..1 (the browser's corner
    tops out at 11 kHz, the C++ at 6.3 kHz, so a bright browser setting cannot be matched)."""
    g = 1 + 110 * drive * drive
    d = min(1.0, max(0.0, (g * 4.7e3 - 51e3) / 500e3))
    fc = 500 * 2 ** (4.5 * tone)
    t = min(1.0, max(0.0, math.sqrt(max(0.0, (fc - 300) / 6000))))
    return d, t


for cid, (d, t, lv), note in (("od-default", (0.5, 0.5, -6), "same knob values on both sides"),
                             ("od-A", (0.2, 0.3, -6), "low drive"),
                             ("od-B", (0.9, 0.7, -10), "high drive")):
    j = dict(drive=d, tone=t, level=lv)
    case(cid, Spec("od", "overdrive", T), j, j, input="riff", note=note)
    md, mt = od_map(d, t)
    case(cid + "-mapped", Spec("od", "overdrive", T), j, dict(drive=round(md, 4), tone=round(mt, 4), level=lv), input="riff", note=f"C++ drive {md:.3f} tone {mt:.3f} give the browser's clipper gain and tone corner")

for cid, (d, t, lv) in (("od-sine-A", (0.3, 0.5, -6)), ("od-sine-B", (0.8, 0.5, -6))):
    j = dict(drive=d, tone=t, level=lv)
    case(cid, Spec("od", "overdrive", T), j, j, input="sine3100", note="3.1 kHz sine at -12 dBFS: harmonics and aliasing", harm=3100)

# references for tools/parity/fit_od.py (a grid search for the C++ knobs closest to each browser setting)
for cid, (d, t) in (("odfit-1", (0.5, 0.5)), ("odfit-2", (0.2, 0.3)), ("odfit-3", (0.9, 0.7))):
    case(cid, Spec("odfit", "overdrive", T), dict(drive=d, tone=t, level=-6), dict(drive=d, tone=t, level=-6), input="riff6", note="grid-search reference")

# ---- reverb: browser Reverb (stereo)  <->  cc.squatch.hall (mono) ----------------------------------
H = "cc.squatch.hall"
rd = dict(decay=2.4, size=1, damp=6500, predelay=18, mix=0.25)
jr = lambda p: dict(p, width=1)
case("reverb-default", Spec("reverb", "reverb", H), jr(rd), rd, input="chords", tail=4, note="defaults are identical; browser left channel at width 1 = the C++ mono output")
ra = dict(decay=1.0, size=0.6, damp=3000, predelay=5, mix=0.4)
case("reverb-A", Spec("reverb", "reverb", H), jr(ra), ra, input="chords", tail=3, note="short and dark")
rb = dict(decay=6, size=1.4, damp=12000, predelay=60, mix=0.3)
case("reverb-B", Spec("reverb", "reverb", H), jr(rb), rb, input="chords", tail=8, note="long and bright; size 1.4 is inside both ranges")
case("reverb-impulse", Spec("reverb", "reverb", H), jr(dict(rd, mix=1)), dict(rd, mix=1), input="impulse", note="mix 1, impulse in: the wet response alone")
rc = dict(decay=2.4, size=1.6, damp=6500, predelay=18, mix=0.25)
case("reverb-size-1.6", Spec("reverb", "reverb", H), jr(rc), rc, input="chords", tail=4, note="size 1.6 is the browser's maximum; the C++ clamps to 1.5, so this shows the range difference")

# ---- limiter: browser Limiter  <->  cc.squatch.limiter ------------------------------------------
L = "cc.squatch.limiter"
ld = dict(gain=0, ceiling=-1, release=120)
lc = lambda j: dict(drive=j["gain"], ceiling=j["ceiling"], release=j["release"], output=0)
case("limiter-default", Spec("limiter", "limiter", L), ld, lc(ld), input="stabs", gain=6, note="+6 dB into the browser default ceiling -1 dB, release 120 on both")
case("limiter-own-defaults", Spec("limiter", "limiter", L), {}, {}, input="stabs", gain=6, note="each side's shipped default (release 120 vs 50)")
la = dict(gain=6, ceiling=-3, release=50)
case("limiter-A", Spec("limiter", "limiter", L), la, lc(la), input="stabs", note="drive 6 dB, ceiling -3, release 50")
lb = dict(gain=12, ceiling=-6, release=300)
case("limiter-B", Spec("limiter", "limiter", L), lb, lc(lb), input="stabs", note="drive 12 dB, ceiling -6, release 300 (heavy)")

lp_ = dict(gain=0, ceiling=-3, release=120)
case("limiter-isp", Spec("limiter", "limiter", L), lp_, lc(lp_), input="fs4", note="12 kHz sine at 45 degrees: samples peak -4 dBFS, waveform -0.9 dBTP; ceiling -3. Only a true-peak limiter acts")

# ---- NAM head  <->  NAM amp, with the MIT NAM Core example models ------------------------------------
for model, tag in (("namcore-example-A1.nam", "A1"), ("namcore-example-A2.nam", "A2")):
    nd = dict(input=0, output=0, bass=0, normalize=1, dcblock=1)
    case(f"nam-{tag}-default", Spec("nam", "nam", "nam"), nd, dict(input=0, output=0), input="riff", jsx=dict(model=model), cppx=dict(model=model, normalize=1), note="browser defaults (DC block on, bass 0, normalize on) vs the C++ adapter's defaults")
    nn = dict(input=0, output=0, bass=0, normalize=1, dcblock=0)
    case(f"nam-{tag}-nodc", Spec("nam", "nam", "nam"), nn, dict(input=0, output=0), input="riff", jsx=dict(model=model), cppx=dict(model=model, normalize=1), note="browser DC block off: the model itself, same normalisation")
    ng = dict(input=6, output=-3, bass=0, normalize=1, dcblock=0)
    case(f"nam-{tag}-gain", Spec("nam", "nam", "nam"), ng, dict(input=6, output=-3), input="riff", jsx=dict(model=model), cppx=dict(model=model, normalize=1), note="input +6 dB, output -3 dB, DC block off")

nb = dict(input=0, output=0, bass=-6, normalize=1, dcblock=0)
case("nam-A1-bass", Spec("nam", "nam", "nam"), nb, dict(input=0, output=0), input="riff", jsx=dict(model="namcore-example-A1.nam"), cppx=dict(model="namcore-example-A1.nam", normalize=1), note="browser Bass -6 dB (low shelf at 160 Hz); the C++ adapter has no counterpart")

# ---- cab: browser Cab (synthesised IR + low/high cut + level)  <->  cc.squatch.cabinet given the SAME IR --------
for cid, v, note in (("cab-open", dict(voice=0, mic=0.5, lowcut=20, highcut=16000, level=0), "cuts at their extremes: the convolver alone"),
                     ("cab-default", dict(voice=0, mic=0.5, lowcut=70, highcut=9000, level=0), "browser defaults: adds a 70 Hz / 9 kHz cut the C++ block lacks"),
                     ("cab-v1", dict(voice=1, mic=0.8, lowcut=20, highcut=16000, level=-6), "2x12 open, bright mic, level -6 dB on both")):
    case(cid, Spec("cab", "cab", "cc.squatch.cabinet"), v, dict(level=v["level"]), input="riff", note=note, jsx=dict(), cppx=dict(ir=True))

(out / "cases.json").write_text(json.dumps(CASES, indent=1))
print(f"{len(CASES)} cases, inputs in {out / 'in'}")
