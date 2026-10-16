<!-- SPDX-License-Identifier: MIT -->
# squatch-dsp CPU benchmark

Apple M1 Max, Apple clang version 21.0.0 (clang-2100.3.34.2), `-O3 -fno-exceptions -fno-rtti`, 2026-09-26.
`make bench` writes this file.

Nanoseconds per sample at 48 kHz, by block size (best of 7 runs of 10 s of audio, one core).
At 48 kHz one core has 20833 ns per sample; the last column is that share at 128.

| block | 16 | 32 | 64 | 128 | % of a core |
|---|---:|---:|---:|---:|---:|
| limiter (limiting) | 69.58 | 69.05 | 68.94 | 68.48 | 0.33% |
| gate (open) | 4.75 | 4.74 | 4.75 | 4.74 | 0.02% |
| eq (3 bands) | 6.30 | 8.23 | 10.62 | 11.86 | 0.06% |
| gate + eq + limiter | 90.76 | 85.79 | 87.69 | 87.77 | 0.42% |
| tuner (tracking 110 Hz) | 134.99 | 134.31 | 134.53 | 134.84 | 0.65% |
| cabinet (200 ms response, partition 64) | 80.19 | 80.32 | 79.76 | 80.10 | 0.39% |
| comp (4:1) | 9.88 | 9.91 | 9.77 | 9.81 | 0.05% |
| trail od (4x oversampled) | 15.80 | 15.57 | 15.48 | 15.34 | 0.07% |
| crunch (high-gain preamp, 4x oversampled) | 153.75 | 152.11 | 152.35 | 152.99 | 0.73% |
| claw (op-amp distortion circuit model, 4x oversampled) | 243.49 | 242.44 | 242.51 | 242.54 | 1.17% |
| hall (FDN reverb, 8 lines) | 13.39 | 13.40 | 13.25 | 13.11 | 0.06% |
