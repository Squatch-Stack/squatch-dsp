<!-- SPDX-License-Identifier: MIT -->
# squatch-dsp cabinet benchmark, long responses

aarch64, g++ (Debian 14.2.0-19) 14.2.0,
`-O3 -fno-exceptions -fno-rtti`. `make bench-long` writes this file.

Conditions (added by hand from the run's own log; the long table did not print them when it ran): Raspberry Pi
5 Model B Rev 1.0, kernel 6.18.50+rpt-rpi-2712, g++ (Debian 14.2.0-19) 14.2.0, flags `-std=c++17
-fno-exceptions -fno-rtti -Wall -Wextra -Wpedantic -Wshadow -Werror -Iinclude -Iblocks/tuner/include -O3`,
governor ondemand (unchanged), run 2 of 3 with the bench to itself, started after the board cooled. Start:
2026-10-20T14:14:18Z temp=62.6'C throttled=0xe0000 arm_clock_hz=1500016128 gov=ondemand load=0.14 0.52 0.63.
End: 2026-10-20T14:20:15Z temp=83.4'C throttled=0xe0008 arm_clock_hz=2311027968 gov=ondemand load=1.01 0.89
0.77.

The NAMBench method as in bench.md (see THIRD_PARTY_NOTICES.md), with passes of 2 s of audio and 6
passes for the block percentile. core_percent is the share of one core; p99 and max are one block's
time as a percent of its own deadline (above 100% a callback was late).

| engine | taps | size | core_percent | block p99 | block max | spread | attempts |
|---|---:|---:|---:|---:|---:|---:|---:|
| old: partition 64 | 4096 | 32 | 0.669% | 1.261% | 1.697% | 0.17% | 1 |
| old: partition 64 | 4096 | 128 | 0.668% | 0.758% | 3.843% | 0.17% | 1 |
| auto (long plan above 16384) | 4096 | 32 | 0.669% | 1.264% | 1.694% | 0.16% | 1 |
| auto (long plan above 16384) | 4096 | 128 | 0.669% | 0.799% | 2.273% | 0.11% | 1 |
| old: partition 64 | 16384 | 32 | 1.699% | 3.347% | 4.636% | 0.09% | 1 |
| old: partition 64 | 16384 | 128 | 1.697% | 1.759% | 2.037% | 0.11% | 1 |
| auto (long plan above 16384) | 16384 | 32 | 1.698% | 3.350% | 15.600% | 0.07% | 1 |
| auto (long plan above 16384) | 16384 | 128 | 1.697% | 1.761% | 2.540% | 0.05% | 1 |
| old: partition 64 | 48000 | 32 | 4.264% | 8.572% | 12.711% | 0.14% | 1 |
| old: partition 64 | 48000 | 128 | 4.261% | 4.372% | 5.047% | 0.04% | 1 |
| auto (long plan above 16384) | 48000 | 32 | 1.449% | 5.650% | 8.322% | 0.41% | 1 |
| auto (long plan above 16384) | 48000 | 128 | 1.440% | 3.191% | 3.285% | 0.05% | 1 |
| old: partition 64 | 240000 | 32 | 25.037% | 67.687% | 81.412% | 2.23% | 5 |
| old: partition 64 | 240000 | 128 | 25.549% | 36.651% | 49.058% | 2.28% | 2 |
| auto (long plan above 16384) | 240000 | 32 | 1.962% | 8.042% | 11.472% | 1.44% | 2 |
| auto (long plan above 16384) | 240000 | 128 | 1.956% | 6.110% | 8.892% | 2.20% | 1 |
