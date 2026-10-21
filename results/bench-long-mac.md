<!-- SPDX-License-Identifier: MIT -->
# squatch-dsp cabinet benchmark, long responses

Apple M1 Max, Apple clang version 21.0.0 (clang-2100.3.34.2),
`-O3 -fno-exceptions -fno-rtti`. `make bench-long` writes this file.

The NAMBench method as in bench.md (see THIRD_PARTY_NOTICES.md), with passes of 2 s of audio and 6
passes for the block percentile. core_percent is the share of one core; p99 and max are one block's
time as a percent of its own deadline (above 100% a callback was late). The cut filters are off.

| engine | taps | size | core_percent | block p99 | block max | spread | attempts |
|---|---:|---:|---:|---:|---:|---:|---:|
| old: partition 64 | 4096 | 32 | 0.263% | 0.462% | 4.419% | 1.14% | 1 |
| old: partition 64 | 4096 | 128 | 0.264% | 0.383% | 2.530% | 1.64% | 1 |
| auto (long plan above 16384) | 4096 | 32 | 0.265% | 0.469% | 7.725% | 1.02% | 1 |
| auto (long plan above 16384) | 4096 | 128 | 0.263% | 0.342% | 2.514% | 0.62% | 1 |
| old: partition 64 | 16384 | 32 | 0.572% | 1.375% | 85.119% | 1.06% | 1 |
| old: partition 64 | 16384 | 128 | 0.573% | 0.970% | 4.656% | 3.00% | 1 |
| auto (long plan above 16384) | 16384 | 32 | 0.573% | 1.100% | 7.013% | 1.25% | 1 |
| auto (long plan above 16384) | 16384 | 128 | 0.573% | 0.889% | 10.050% | 2.46% | 1 |
| old: partition 64 | 48000 | 32 | 1.264% | 2.844% | 46.137% | 0.93% | 1 |
| old: partition 64 | 48000 | 128 | 1.260% | 3.083% | 71.873% | 1.52% | 1 |
| auto (long plan above 16384) | 48000 | 32 | 0.725% | 2.731% | 30.925% | 2.23% | 1 |
| auto (long plan above 16384) | 48000 | 128 | no run agreed within 3% in 5 attempts | | | | |
| old: partition 64 | 240000 | 32 | 5.932% | 18.944% | 65.681% | 2.85% | 4 |
| old: partition 64 | 240000 | 128 | 6.064% | 11.679% | 19.448% | 1.64% | 2 |
| auto (long plan above 16384) | 240000 | 32 | 0.855% | 4.150% | 32.344% | 2.53% | 1 |
| auto (long plan above 16384) | 240000 | 128 | 0.848% | 2.342% | 8.853% | 1.35% | 2 |
