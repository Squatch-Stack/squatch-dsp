<!-- SPDX-License-Identifier: MIT -->
# squatch-dsp cabinet benchmark, long responses

aarch64, g++ (Debian 14.2.0-19) 14.2.0,
`-O3 -fno-exceptions -fno-rtti`. `make bench-long` writes this file.

Conditions (added by hand from the run's own log; the long table did not print them when it ran): Raspberry Pi
5 Model B Rev 1.0, kernel 6.18.50+rpt-rpi-2712, g++ (Debian 14.2.0-19) 14.2.0, flags `-std=c++17
-fno-exceptions -fno-rtti -Wall -Wextra -Wpedantic -Wshadow -Werror -Iinclude -Iblocks/tuner/include -O3`,
governor ondemand (unchanged), run 1 of 3 with the bench to itself, started after the board cooled. Start:
2026-10-20T13:53:30Z temp=62.6'C throttled=0xe0000 arm_clock_hz=1500019456 gov=ondemand load=0.16 0.47 0.76.
End: 2026-10-20T13:58:24Z temp=83.4'C throttled=0xe0008 arm_clock_hz=2256019456 gov=ondemand load=1.15 0.89
0.87.

The NAMBench method as in bench.md (see THIRD_PARTY_NOTICES.md), with passes of 2 s of audio and 6
passes for the block percentile. core_percent is the share of one core; p99 and max are one block's
time as a percent of its own deadline (above 100% a callback was late).

| engine | taps | size | core_percent | block p99 | block max | spread | attempts |
|---|---:|---:|---:|---:|---:|---:|---:|
| old: partition 64 | 4096 | 32 | 0.670% | 1.264% | 2.586% | 0.06% | 1 |
| old: partition 64 | 4096 | 128 | 0.670% | 0.681% | 1.744% | 0.12% | 1 |
| auto (long plan above 16384) | 4096 | 32 | 0.670% | 1.267% | 14.531% | 0.30% | 1 |
| auto (long plan above 16384) | 4096 | 128 | 0.670% | 0.740% | 1.767% | 0.12% | 1 |
| old: partition 64 | 16384 | 32 | 1.699% | 3.542% | 8.211% | 0.14% | 1 |
| old: partition 64 | 16384 | 128 | 1.696% | 1.766% | 2.547% | 0.05% | 1 |
| auto (long plan above 16384) | 16384 | 32 | 1.700% | 3.350% | 17.206% | 0.51% | 1 |
| auto (long plan above 16384) | 16384 | 128 | 1.697% | 2.126% | 3.693% | 0.08% | 1 |
| old: partition 64 | 48000 | 32 | 4.269% | 8.575% | 22.550% | 0.29% | 1 |
| old: partition 64 | 48000 | 128 | 4.266% | 4.384% | 5.193% | 0.40% | 1 |
| auto (long plan above 16384) | 48000 | 32 | 1.450% | 5.631% | 8.339% | 0.08% | 1 |
| auto (long plan above 16384) | 48000 | 128 | 1.443% | 3.190% | 3.299% | 0.90% | 1 |
| old: partition 64 | 240000 | 32 | 21.335% | 58.534% | 82.634% | 1.98% | 1 |
| old: partition 64 | 240000 | 128 | 25.763% | 33.241% | 40.607% | 2.10% | 4 |
| auto (long plan above 16384) | 240000 | 32 | no run agreed within 3% in 5 attempts | | | | |
| auto (long plan above 16384) | 240000 | 128 | 1.920% | 5.719% | 6.679% | 2.83% | 4 |
