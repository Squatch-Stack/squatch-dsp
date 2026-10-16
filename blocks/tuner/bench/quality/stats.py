# SPDX-License-Identifier: MIT
"""The statistics the scorecard's breakdowns use (standard library only): quantiles, Friedman's test
with ties and Kendall's W, and a bootstrap interval on a median of paired differences."""
import math
import random
from statistics import median


def q(v, p):
    """The p-quantile of v by linear interpolation, or None for no values."""
    if not v:
        return None
    s = sorted(v)
    i = p * (len(s) - 1)
    lo = int(i)
    return s[lo] if lo + 1 >= len(s) else s[lo] * (1 - (i - lo)) + s[lo + 1] * (i - lo)


def summary(v):
    return {"median": q(v, 0.5), "p90": q(v, 0.9), "p95": q(v, 0.95), "n": len(v)}


def _gamma_series(a, x, lg):
    s, term, n = 1.0 / a, 1.0 / a, a
    for _ in range(500):
        n += 1
        term *= x / n
        s += term
        if abs(term) < abs(s) * 1e-14:
            break
    return max(0.0, 1.0 - s * math.exp(-x + a * math.log(x) - lg))


def _tiny(v):
    return 1e-300 if abs(v) < 1e-300 else v


def _gamma_fraction(a, x, lg):
    b, c, d = x + 1 - a, 1e300, 1 / (x + 1 - a)
    h = d
    for i in range(1, 500):
        an = -i * (i - a)
        b += 2
        d = 1 / _tiny(an * d + b)
        c = _tiny(b + an / c)
        h *= d * c
        if abs(d * c - 1) < 1e-14:
            break
    return math.exp(-x + a * math.log(x) - lg) * h


def gammaincc(a, x):
    """Regularised upper incomplete gamma Q(a, x) (series below a + 1, continued fraction above)."""
    if x <= 0:
        return 1.0
    lg = math.lgamma(a)
    return _gamma_series(a, x, lg) if x < a + 1 else _gamma_fraction(a, x, lg)


def ranks(row):
    """Ranks from 1, ties sharing their mean rank."""
    order = sorted(range(len(row)), key=lambda i: row[i])
    r = [0.0] * len(row)
    i = 0
    while i < len(order):
        j = i
        while j + 1 < len(order) and row[order[j + 1]] == row[order[i]]:
            j += 1
        for k in range(i, j + 1):
            r[order[k]] = (i + j) / 2 + 1
        i = j + 1
    return r


def friedman(blocks):
    """Friedman's test on complete blocks (one list per note, one value per position), with ties.
    Returns (chi2, p, Kendall's W)."""
    n, k = len(blocks), len(blocks[0]) if blocks else 0
    if n < 3 or k < 2:
        return None, None, None
    rs = [ranks(b) for b in blocks]
    mean = (k + 1) / 2
    sst = n * sum((sum(r[j] for r in rs) / n - mean) ** 2 for j in range(k))
    sse = sum((x - mean) ** 2 for r in rs for x in r) / (n * (k - 1))
    if sse <= 0:
        return 0.0, 1.0, 0.0
    chi2 = sst / sse
    return chi2, gammaincc((k - 1) / 2, chi2 / 2), chi2 / (n * (k - 1))


def paired(diffs, seed=7, n=2000):
    """Median of paired differences and its bootstrap 95% interval."""
    if len(diffs) < 5:
        return None
    rng = random.Random(seed)
    boots = sorted(median(rng.choice(diffs) for _ in diffs) for _ in range(n))
    return {"median": median(diffs), "lo": boots[int(0.025 * n)], "hi": boots[int(0.975 * n) - 1], "n": len(diffs),
            "mean": sum(diffs) / len(diffs)}
