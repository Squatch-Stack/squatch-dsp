# Squatch Tuner: real DI accuracy

Real DI: 275 notes that ring at least 1.5 s, from 2 file(s); each algorithm runs continuously
over the whole file; re-pitch test +7.0 c.

| algorithm | notes | octave/gross slips | re-pitch error median c | abs p50 c | abs p95 c | jitter p50 c | stable ±1c p50/p90 ms | never | stable ±0.1c p50/p90 ms | never |
|---|---|---|---|---|---|---|---|---|---|---|
| YIN (12k, median-5) | 272 | 0 | 0.027 | 0.130 | 0.505 | 0.400 | 776 / 1462 | 52 | 1493 / 1499 | 233 |
| MPM (12k, median-5) | 272 | 0 | 0.003 | 0.041 | 0.250 | 0.369 | 704 / 1141 | 41 | 1483 / 1499 | 225 |
| MPM + phase vocoder | 272 | 0 | -0.001 | 0.058 | 0.293 | 0.527 | 1216 / 1493 | 79 | 1493 / 1499 | 244 |
| Squatch strobe (hz, now) | 270 | 1 | 0.000 | 0.008 | 0.176 | 0.510 | 864 / 1262 | 54 | 1435 / 1492 | 206 |
| Squatch strobe (steadyHz) | 270 | 1 | 0.001 | 0.005 | 0.131 | 0.382 | 715 / 1182 | 30 | 1168 / 1476 | 205 |

Pitch after the pluck relative to the settled pitch (1.0-1.5 s), Squatch strobe trace, cents:

| after | p10 | p50 | p90 |
|---|---|---|---|
| 50 ms | -0.76 | 5.22 | 15.00 |
| 100 ms | -2.82 | 2.06 | 7.85 |
| 200 ms | -3.62 | 0.85 | 5.25 |
| 300 ms | -3.21 | 0.22 | 4.16 |
| 500 ms | -2.36 | -0.06 | 2.23 |
| 750 ms | -1.77 | -0.17 | 1.59 |
