<!-- SPDX-License-Identifier: MIT -->
# squatch-dsp CPU benchmark

Apple M1 Max, Apple clang version 21.0.0 (clang-2100.3.34.2), `-O3 -fno-exceptions -fno-rtti`, 2026-10-20.
`make bench` writes this file.

Nanoseconds per sample at 48 kHz, by block size (best of 7 runs of 10 s of audio, one core).
At 48 kHz one core has 20833 ns per sample; the last column is that share at 128.

| block | 16 | 32 | 64 | 128 | % of a core |
|---|---:|---:|---:|---:|---:|
| limiter (limiting) | 87.18 | 86.41 | 86.49 | 86.78 | 0.41% |
| gate (open) | 7.26 | 7.19 | 7.10 | 7.05 | 0.03% |
| eq (3 bands) | 6.32 | 8.43 | 10.85 | 11.93 | 0.06% |
| gate + eq + limiter | 88.33 | 87.98 | 90.05 | 91.80 | 0.44% |
| tuner (tracking 110 Hz) | 136.63 | 136.47 | 136.79 | 136.34 | 0.66% |
| cabinet (200 ms response, partition 64) | 92.35 | 92.19 | 91.74 | 91.62 | 0.44% |
| comp (4:1) | 10.04 | 9.88 | 9.88 | 9.95 | 0.05% |
| trail od (4x oversampled) | 194.75 | 193.02 | 191.77 | 192.60 | 0.92% |
| crunch (high-gain preamp, 4x oversampled) | 156.42 | 157.66 | 156.77 | 157.38 | 0.75% |
| claw (op-amp distortion circuit model, 4x oversampled) | 251.88 | 250.68 | 250.86 | 250.96 | 1.20% |
| hall (FDN reverb, 8 lines) | 14.33 | 13.99 | 13.82 | 13.66 | 0.07% |

Same blocks, measured with the NAMBench method (pedal-science_NAMBench, MIT;
see THIRD_PARTY_NOTICES.md). A pass is 10 s of audio, repeated until it lasts 20 ms.
The kept passes are the tightest 70% of those timed in 1 s; their spread must be within 3%,
else the attempt is voided and rerun. core_percent = mean pass time / audio time.
p99 and max are one block's time as a percent of its own deadline (block size / 48 kHz)
over 20 passes; above 100% a callback missed its deadline.

| block | size | core_percent | block p99 | block max | spread | attempts |
|---|---:|---:|---:|---:|---:|---:|
| limiter (limiting) | 16 | 0.420% | 0.487% | 40.288% | 2.83% | 1 |
| limiter (limiting) | 32 | 0.420% | 0.475% | 225.619% | 1.60% | 1 |
| limiter (limiting) | 64 | 0.419% | 0.472% | 31.116% | 2.14% | 1 |
| limiter (limiting) | 128 | 0.419% | 0.620% | 9.872% | 1.24% | 1 |
| gate (open) | 16 | 0.036% | 0.037% | 83.900% | 1.43% | 1 |
| gate (open) | 32 | 0.035% | 0.037% | 6.562% | 1.71% | 1 |
| gate (open) | 64 | 0.035% | 0.037% | 4.000% | 2.65% | 1 |
| gate (open) | 128 | 0.035% | 0.036% | 2.892% | 1.91% | 1 |
| eq (3 bands) | 16 | 0.031% | 0.088% | 38.875% | 1.74% | 1 |
| eq (3 bands) | 32 | 0.041% | 0.050% | 15.663% | 2.45% | 1 |
| eq (3 bands) | 64 | 0.053% | 0.056% | 7.431% | 2.71% | 2 |
| eq (3 bands) | 128 | 0.059% | 0.063% | 7.828% | 2.12% | 1 |
| gate + eq + limiter | 16 | 0.427% | 0.513% | 65.912% | 1.32% | 1 |
| gate + eq + limiter | 32 | 0.422% | 0.481% | 267.562% | 1.39% | 1 |
| gate + eq + limiter | 64 | 0.433% | 0.491% | 18.062% | 1.79% | 1 |
| gate + eq + limiter | 128 | 0.442% | 0.553% | 8.534% | 1.39% | 1 |
| tuner (tracking 110 Hz) | 16 | 0.671% | 32.375% | 165.025% | 1.44% | 1 |
| tuner (tracking 110 Hz) | 32 | 0.668% | 16.862% | 155.419% | 0.98% | 1 |
| tuner (tracking 110 Hz) | 64 | 0.668% | 8.550% | 33.053% | 1.29% | 1 |
| tuner (tracking 110 Hz) | 128 | 0.666% | 4.538% | 14.492% | 1.27% | 1 |
| cabinet (200 ms response, partition 64) | 16 | 0.448% | 1.500% | 142.200% | 1.62% | 1 |
| cabinet (200 ms response, partition 64) | 32 | 0.445% | 0.838% | 38.475% | 1.43% | 1 |
| cabinet (200 ms response, partition 64) | 64 | 0.442% | 0.553% | 77.869% | 2.11% | 1 |
| cabinet (200 ms response, partition 64) | 128 | 0.444% | 0.675% | 11.506% | 1.39% | 1 |
