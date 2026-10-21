<!-- SPDX-License-Identifier: MIT -->
# squatch-dsp cabinet benchmark, long responses

aarch64, g++ (Debian 14.2.0-19) 14.2.0,
`-O3 -fno-exceptions -fno-rtti`. `make bench-long` writes this file.

Conditions (added by hand from the run's own log; the long table did not print them when it ran): Raspberry Pi
5 Model B Rev 1.0, kernel 6.18.50+rpt-rpi-2712, g++ (Debian 14.2.0-19) 14.2.0, flags `-std=c++17
-fno-exceptions -fno-rtti -Wall -Wextra -Wpedantic -Wshadow -Werror -Iinclude -Iblocks/tuner/include -O3`,
governor ondemand (unchanged), run 3 of 3 with the bench to itself, started after the board cooled. Start:
2026-10-20T14:59:55Z temp=62.6'C throttled=0xe0000 arm_clock_hz=1500022656 gov=ondemand load=0.17 0.61 0.71.
End: 2026-10-20T15:03:01Z temp=83.4'C throttled=0xe0008 arm_clock_hz=2256022784 gov=ondemand load=0.98 0.81
0.78.

The NAMBench method as in bench.md (see THIRD_PARTY_NOTICES.md), with passes of 2 s of audio and 6
passes for the block percentile. core_percent is the share of one core; p99 and max are one block's
time as a percent of its own deadline (above 100% a callback was late).

| engine | taps | size | core_percent | block p99 | block max | spread | attempts |
|---|---:|---:|---:|---:|---:|---:|---:|
| old: partition 64 | 4096 | 32 | 0.671% | 1.267% | 2.347% | 0.16% | 1 |
| old: partition 64 | 4096 | 128 | 0.670% | 0.678% | 0.985% | 0.13% | 1 |
| auto (long plan above 16384) | 4096 | 32 | 0.671% | 1.269% | 1.803% | 0.13% | 1 |
| auto (long plan above 16384) | 4096 | 128 | 0.670% | 0.678% | 0.784% | 0.14% | 1 |
| old: partition 64 | 16384 | 32 | 1.698% | 3.345% | 4.003% | 0.06% | 1 |
| old: partition 64 | 16384 | 128 | 1.697% | 1.764% | 2.139% | 0.03% | 1 |
| auto (long plan above 16384) | 16384 | 32 | 1.698% | 3.347% | 5.336% | 0.06% | 1 |
| auto (long plan above 16384) | 16384 | 128 | 1.697% | 1.765% | 1.870% | 0.06% | 1 |
| old: partition 64 | 48000 | 32 | 4.293% | 8.670% | 10.858% | 0.03% | 1 |
| old: partition 64 | 48000 | 128 | 4.296% | 4.408% | 4.888% | 0.05% | 1 |
| auto (long plan above 16384) | 48000 | 32 | 1.460% | 5.625% | 8.870% | 0.05% | 1 |
| auto (long plan above 16384) | 48000 | 128 | 1.451% | 3.194% | 3.315% | 0.05% | 1 |
| old: partition 64 | 240000 | 32 | 21.321% | 52.126% | 74.567% | 1.31% | 1 |
| old: partition 64 | 240000 | 128 | 23.674% | 34.061% | 38.589% | 2.72% | 2 |
| auto (long plan above 16384) | 240000 | 32 | 1.896% | 7.683% | 11.995% | 2.39% | 2 |
| auto (long plan above 16384) | 240000 | 128 | 1.868% | 5.588% | 5.988% | 1.69% | 2 |
