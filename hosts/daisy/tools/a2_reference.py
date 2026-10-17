#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Float64 ground truth for an A2 WaveNet, and each build's error against it.

    a2_reference.py <model.nam> --slim V --out ref.f64
    a2_reference.py <model.nam> --slim V --score ref.f64 a.f64 b.f64 ...

The C++ paths all compute in float32, so comparing one against another only
says which rounding they share. This computes the same network in float64,
straight from the .nam weights, as the independent reference: the "loss" of a
build is its error against this, and a change is lossless if it does not raise
that error.

Used by nam_check.py and nam_bench.py with --input: the exact float bits the
board was fed, so only arithmetic error is scored. State matches a reset:
the signal is preceded by two receptive fields of silence. Written for the
A2-Lite shape (3 channels, 23 layers); the weights are read in NAM Core's order.
"""
import argparse
import json
import math
import sys

import numpy as np

KS = [6] * 14 + [15, 15] + [6] * 7
DIL = [1, 3, 7, 17, 41, 101, 239] * 2 + [1, 13] + [1, 3, 7, 17, 41, 101, 239]
HEAD_K = 16
SLOPE = 0.01


def pick(j, slim):
    if j["architecture"] != "SlimmableContainer":
        return j
    subs = j["config"]["submodels"]
    for s in subs:
        if slim < s["max_value"]:
            return s["model"]
    return subs[-1]["model"]


def test_signal(sr=48000.0):
    """A deterministic test signal (log sweep, then LCG noise), used when no --input is given."""
    n = int(5.0 * sr)
    t = np.arange(n) / sr
    f0, f1, T = 40.0, 12000.0, 5.0
    k = math.log(f1 / f0)
    sweep = 0.5 * np.sin(2.0 * math.pi * f0 * T / k * (np.exp(t / T * k) - 1.0))
    s = 0x5EED1234
    noise = np.empty(n)
    for i in range(n):
        s = (s * 1664525 + 1013904223) & 0xFFFFFFFF
        noise[i] = 0.3 * (s / 4294967296.0 * 2.0 - 1.0)
    return np.concatenate([sweep, noise])


def load(model):
    """Weights in the fast path's reading order (NAM Core's NAM/wavenet/a2_fast.cpp, _load_weights)."""
    C = model["config"]["layers"][0]["channels"]
    w = iter(np.asarray(model["weights"], dtype=np.float64))
    take = lambda n: np.array([next(w) for _ in range(n)])
    P = {"C": C, "rechannel": take(C), "layers": []}
    for K in KS:
        conv = take(C * C * K).reshape(C, C, K)  # [out][in][tap]
        P["layers"].append(dict(
            conv=conv, conv_b=take(C), mixin=take(C),
            l1x1=take(C * C).reshape(C, C), l1x1_b=take(C)))  # [out][in]
    P["head"] = take(C * HEAD_K).reshape(C, HEAD_K)  # [in][tap]
    P["head_b"] = next(w)
    P["head_scale"] = next(w)
    assert next(w, None) is None, "trailing weights"
    return P


def forward(P, x):
    """x: 1-D float64 input. Returns 1-D float64 output (same length)."""
    C, N = P["C"], len(x)
    cond = x
    h = np.outer(P["rechannel"], x)  # layer input, C x N
    head_sum = np.zeros((C, N))
    for L, K, D in zip(P["layers"], KS, DIL):
        z = np.zeros((C, N))
        for k in range(K):
            shift = (K - 1 - k) * D
            xs = np.zeros((C, N))
            xs[:, shift:] = h[:, : N - shift]
            z += L["conv"][:, :, k] @ xs
        z += L["conv_b"][:, None]
        z += np.outer(L["mixin"], cond)
        a = np.where(z >= 0, z, z * SLOPE)
        head_sum += a
        h = h + L["l1x1"] @ a + L["l1x1_b"][:, None]
    y = np.full(N, P["head_b"])
    for k in range(HEAD_K):
        shift = HEAD_K - 1 - k
        hs = np.zeros((C, N))
        hs[:, shift:] = head_sum[:, : N - shift]
        y += P["head"][:, k] @ hs
    return y * P["head_scale"]


def score(ref, path):
    y = np.fromfile(path, dtype="<f8")
    n = min(len(y), len(ref))
    e = y[:n] - ref[:n]
    esr = 10 * math.log10(np.sum(e * e) / np.sum(ref[:n] ** 2))
    return float(np.max(np.abs(e))), esr


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("model")
    ap.add_argument("--slim", type=float, default=1.0)
    ap.add_argument("--out")
    ap.add_argument("--input", help="raw little-endian float64 input (else the built-in test signal)")
    ap.add_argument("--score", nargs="+")
    a = ap.parse_args()
    if a.score:
        ref = np.fromfile(a.score[0], dtype="<f8")
        print(f"{'build':44} {'max|err| vs float64':>20} {'ESR vs float64':>15}")
        for p in a.score[1:]:
            mx, esr = score(ref, p)
            print(f"{p:44} {mx:20.3e} {esr:13.2f}dB")
        return 0
    m = pick(json.load(open(a.model)), a.slim)
    P = load(m)
    if a.input:
        x = np.fromfile(a.input, dtype="<f8")
    else:
        x = test_signal().astype(np.float32).astype(np.float64)
    rf = 1 + sum((k - 1) * d for k, d in zip(KS, DIL)) + HEAD_K - 1
    pad = 2 * rf
    y = forward(P, np.concatenate([np.zeros(pad), x]))[pad:]
    y.astype("<f8").tofile(a.out)
    print(f"reference: {P['C']} channels, receptive field {rf}, {len(y)} samples -> {a.out}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
