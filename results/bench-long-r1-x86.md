<!-- SPDX-License-Identifier: MIT -->
# squatch-dsp cabinet benchmark, long responses

x86_64, g++ (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0,
`-O3 -fno-exceptions -fno-rtti`. `make bench-long` writes this file.

Conditions (added by hand from the run's own log; the long table did not print them when it ran): AMD Ryzen 9
9950X, Ubuntu 24.04, kernel 7.2.6-76070206-generic, g++ (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0, flags
`-std=c++17 -fno-exceptions -fno-rtti -Wall -Wextra -Wpedantic -Wshadow -Werror -Iinclude
-Iblocks/tuner/include -O3`, governor powersave (balance_performance EPP, unchanged), run 1 of 3, unpinned, on
a shared x86 server. Start: 2026-10-20T13:23:04Z gov=powersave load=7.09 4.19 2.87 gpu=100 %, 24494 MiB
top=python3:110.0% python3:80.0% systemd:0.0% . End: 2026-10-20T13:24:02Z gov=powersave load=4.61 4.00 2.90
gpu=100 %, 24514 MiB top=python3:83.3% python3:50.0% systemd:0.0% .

The NAMBench method as in bench.md (see THIRD_PARTY_NOTICES.md), with passes of 2 s of audio and 6
passes for the block percentile. core_percent is the share of one core; p99 and max are one block's
time as a percent of its own deadline (above 100% a callback was late).

| engine | taps | size | core_percent | block p99 | block max | spread | attempts |
|---|---:|---:|---:|---:|---:|---:|---:|
| old: partition 64 | 4096 | 32 | 0.193% | 0.354% | 1.199% | 0.38% | 1 |
| old: partition 64 | 4096 | 128 | 0.193% | 0.218% | 0.326% | 0.34% | 1 |
| auto (long plan above 16384) | 4096 | 32 | 0.193% | 0.354% | 1.241% | 0.21% | 1 |
| auto (long plan above 16384) | 4096 | 128 | 0.193% | 0.208% | 0.361% | 0.38% | 1 |
| old: partition 64 | 16384 | 32 | 0.465% | 0.907% | 1.860% | 0.28% | 1 |
| old: partition 64 | 16384 | 128 | 0.465% | 0.529% | 0.651% | 0.27% | 1 |
| auto (long plan above 16384) | 16384 | 32 | 0.465% | 0.905% | 1.479% | 0.32% | 1 |
| auto (long plan above 16384) | 16384 | 128 | 0.465% | 0.514% | 0.659% | 0.27% | 1 |
| old: partition 64 | 48000 | 32 | 1.169% | 2.433% | 5.654% | 0.38% | 1 |
| old: partition 64 | 48000 | 128 | 1.168% | 1.236% | 1.653% | 0.34% | 1 |
| auto (long plan above 16384) | 48000 | 32 | 0.373% | 1.385% | 2.267% | 0.19% | 1 |
| auto (long plan above 16384) | 48000 | 128 | 0.371% | 0.949% | 1.413% | 0.41% | 1 |
| old: partition 64 | 240000 | 32 | 5.790% | 12.299% | 23.562% | 0.17% | 1 |
| old: partition 64 | 240000 | 128 | 5.792% | 6.025% | 6.283% | 0.10% | 1 |
| auto (long plan above 16384) | 240000 | 32 | 0.445% | 1.701% | 3.282% | 0.79% | 1 |
| auto (long plan above 16384) | 240000 | 128 | 0.442% | 1.479% | 1.608% | 0.36% | 1 |
