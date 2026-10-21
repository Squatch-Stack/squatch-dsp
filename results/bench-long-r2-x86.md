<!-- SPDX-License-Identifier: MIT -->
# squatch-dsp cabinet benchmark, long responses

x86_64, g++ (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0,
`-O3 -fno-exceptions -fno-rtti`. `make bench-long` writes this file.

Conditions (added by hand from the run's own log; the long table did not print them when it ran): AMD Ryzen 9
9950X, Ubuntu 24.04, kernel 7.2.6-76070206-generic, g++ (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0, flags
`-std=c++17 -fno-exceptions -fno-rtti -Wall -Wextra -Wpedantic -Wshadow -Werror -Iinclude
-Iblocks/tuner/include -O3`, governor powersave (balance_performance EPP, unchanged), run 2 of 3, unpinned, on
a shared x86 server. Start: 2026-10-20T13:28:29Z gov=powersave load=3.11 3.49 2.97 gpu=100 %, 24276 MiB
top=python3:100.0% python3:70.0% systemd:0.0% . End: 2026-10-20T13:29:26Z gov=powersave load=3.32 3.47 3.00
gpu=100 %, 24276 MiB top=python3:100.0% top:9.1% systemd:0.0% .

The NAMBench method as in bench.md (see THIRD_PARTY_NOTICES.md), with passes of 2 s of audio and 6
passes for the block percentile. core_percent is the share of one core; p99 and max are one block's
time as a percent of its own deadline (above 100% a callback was late).

| engine | taps | size | core_percent | block p99 | block max | spread | attempts |
|---|---:|---:|---:|---:|---:|---:|---:|
| old: partition 64 | 4096 | 32 | 0.192% | 0.354% | 1.411% | 0.31% | 1 |
| old: partition 64 | 4096 | 128 | 0.192% | 0.205% | 0.412% | 0.35% | 1 |
| auto (long plan above 16384) | 4096 | 32 | 0.192% | 0.351% | 2.127% | 0.30% | 1 |
| auto (long plan above 16384) | 4096 | 128 | 0.192% | 0.205% | 0.264% | 0.21% | 1 |
| old: partition 64 | 16384 | 32 | 0.465% | 0.905% | 3.587% | 0.36% | 1 |
| old: partition 64 | 16384 | 128 | 0.464% | 0.549% | 0.676% | 0.33% | 1 |
| auto (long plan above 16384) | 16384 | 32 | 0.466% | 0.905% | 2.910% | 0.62% | 1 |
| auto (long plan above 16384) | 16384 | 128 | 0.464% | 0.510% | 1.390% | 0.38% | 1 |
| old: partition 64 | 48000 | 32 | 1.171% | 2.367% | 3.798% | 0.49% | 1 |
| old: partition 64 | 48000 | 128 | 1.174% | 1.257% | 1.536% | 0.28% | 1 |
| auto (long plan above 16384) | 48000 | 32 | 0.370% | 1.421% | 3.209% | 0.49% | 1 |
| auto (long plan above 16384) | 48000 | 128 | 0.366% | 1.043% | 1.726% | 0.69% | 1 |
| old: partition 64 | 240000 | 32 | 5.811% | 12.009% | 13.817% | 0.24% | 1 |
| old: partition 64 | 240000 | 128 | 5.807% | 5.904% | 6.498% | 0.09% | 1 |
| auto (long plan above 16384) | 240000 | 32 | 0.436% | 1.682% | 3.174% | 0.16% | 1 |
| auto (long plan above 16384) | 240000 | 128 | 0.433% | 1.454% | 1.536% | 0.20% | 1 |
