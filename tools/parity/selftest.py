#!/usr/bin/env python3
"""Checks of the measurement itself, each with a negative control:  python3 -I selftest.py

A known difference is built into a synthetic pair and the analysis must report it; then the same
check is run against a wrong expectation (or an unrelated pair) and must FAIL, so a passing run
means the check can fail.  Exit status is the number of failed checks."""
import importlib.util
import json
import os
import sys
import tempfile
from pathlib import Path

import numpy as np

spec = importlib.util.spec_from_file_location("analyze", Path(__file__).with_name("analyze.py"))
A = importlib.util.module_from_spec(spec)
spec.loader.exec_module(A)
SR = A.SR
rng = np.random.default_rng(7)
fails = 0


def check(name, fn):
    global fails
    try:
        fn()
        print("ok   ", name)
    except AssertionError as e:
        fails += 1
        print("FAIL ", name, e)


def must_fail(name, fn):
    """The negative control: the same check against a wrong expectation has to raise."""
    global fails
    try:
        fn()
    except AssertionError:
        print("ok   ", name, "(fails as it should)")
        return
    fails += 1
    print("FAIL ", name, "passed when it must not")


noise = rng.standard_normal(5 * SR) * 0.1


def lag_case(expect):
    delayed = np.concatenate([np.zeros(10), noise])[: len(noise)]
    got = A.lag_of(noise, delayed)
    assert got == expect, f"lag {got}, wanted {expect}"


check("cross-correlation finds a 10-sample delay", lambda: lag_case(10))
must_fail("negative control: a wrong expected lag", lambda: lag_case(11))


def unrelated():
    assert A.lag_of(noise, rng.standard_normal(len(noise)) * 0.1) == 10, "no peak at 10"


must_fail("negative control: unrelated noise shows no lag of 10", unrelated)


def level(expect):
    d = A.db(A.rms(noise * 10 ** (1 / 20))) - A.db(A.rms(noise))
    assert abs(d - expect) < 0.01, f"level {d}"


check("level difference of +1 dB", lambda: level(1.0))
must_fail("negative control: wrong level expectation", lambda: level(2.0))


def spec_case(expect):
    d = A.spectrum_diff(noise, noise * 2)
    assert abs(d["median_abs"] - expect) < 0.05 and abs(d["max_abs"] - expect) < 0.3, d


check("third-octave difference of a x2 gain is 6.02 dB in every band", lambda: spec_case(6.02))
must_fail("negative control: wrong band expectation", lambda: spec_case(3.0))


def shelf_case(expect_hz_lo, expect_hz_hi):
    spec = np.fft.rfft(noise)
    f = np.fft.rfftfreq(len(noise), 1 / SR)
    spec[(f > 2000) & (f < 4000)] *= 10 ** (3 / 20)
    d = A.spectrum_diff(noise, np.fft.irfft(spec, len(noise)))
    assert expect_hz_lo <= d["max_at_hz"] <= expect_hz_hi and 2.0 < d["max_abs"] < 3.2, d


check("a +3 dB bump at 2-4 kHz is found there", lambda: shelf_case(2000, 4000))
must_fail("negative control: bump looked for at 500 Hz", lambda: shelf_case(400, 600))


def gain_curve(expect):
    x = noise[: SR]
    g = A.window_gain(x * 0.5, x)
    assert abs(np.nanmedian(g) - expect) < 0.05, np.nanmedian(g)


check("window gain of x0.5 is -6.02 dB", lambda: gain_curve(-6.02))
must_fail("negative control: wrong window gain", lambda: gain_curve(-3.0))


def harm(expect):
    n = np.arange(SR)
    y = np.sin(2 * np.pi * 1000 * n / SR) + 0.01 * np.sin(2 * np.pi * 2000 * n / SR)  # H2 at -40 dB
    h, nonh = A.harmonics(y, 1000)
    assert abs(h[0] - expect) < 0.5 and nonh < -80, (h[0], nonh)


check("harmonic analysis reads a -40 dB second harmonic", lambda: harm(-40.0))
must_fail("negative control: wrong harmonic level", lambda: harm(-20.0))


def tp(expect):
    n = np.arange(SR)
    y = np.sin(2 * np.pi * 12000 * n / SR + np.pi / 4) * np.minimum(1, np.minimum(n, n[::-1]) / 4800)
    got = A.true_peak(y)
    assert abs(got - expect) < 0.1 and A.db(np.abs(y).max()) < -2.9, (got, A.db(np.abs(y).max()))


check("true peak of an fs/4 sine at 45 deg: 0 dBTP while samples peak at -3 dBFS", lambda: tp(0.0))
must_fail("negative control: true peak expected at the sample peak", lambda: tp(-3.0))


def rt60(expect):
    t = np.arange(4 * SR) / SR
    y = rng.standard_normal(len(t)) * 10 ** (-3 * t / 1.0)  # -60 dB in 1 s
    got = A.rt60_t20(y, 0)
    assert abs(got - expect) < 0.1, got


check("T20 decay estimate of a 1 s RT60", lambda: rt60(1.0))
must_fail("negative control: wrong RT60", lambda: rt60(2.0))


def end_to_end(scale, esr, esr_gain, level_db):
    with tempfile.TemporaryDirectory(dir=os.environ.get("TMPDIR")) as d:
        d = Path(d)
        for sub in ("in", "js", "cpp"):
            (d / sub).mkdir()
        x = (rng.standard_normal(2 * SR) * 0.1).astype("<f4")
        x.tofile(d / "in" / "x.f32")
        x.tofile(d / "js" / "t.f32")
        (x * np.float32(scale)).astype("<f4").tofile(d / "cpp" / "t.f32")
        (d / "cases.json").write_text(json.dumps([dict(id="t", pair="comp", input="x", gainDb=0, tail=0, note="", harm=None)]))
        (d / "cpp_latency.json").write_text(json.dumps({"t": 0}))
        (d / "js.log").write_text(json.dumps({"id": "t", "latency": 0}) + "\n")
        import contextlib, io
        with contextlib.redirect_stdout(io.StringIO()):
            A.main(d)
        r = json.loads((d / "results.json").read_text())[0]
        assert abs(r["esr"] - esr) < 1e-3 and abs(r["esr_gain"] - esr_gain) < 1e-6 and abs(r["level_db"] - level_db) < 0.01, r


check("whole pipeline: identical files give ESR 0", lambda: end_to_end(1.0, 0.0, 0.0, 0.0))
check("whole pipeline: a x0.5 copy is ESR 0.25, ESR 0 after the best gain, -6.02 dB", lambda: end_to_end(0.5, 0.25, 0.0, -6.0206))
must_fail("negative control: the x0.5 copy is not ESR 0", lambda: end_to_end(0.5, 0.0, 0.0, -6.0206))
sys.exit(fails)
