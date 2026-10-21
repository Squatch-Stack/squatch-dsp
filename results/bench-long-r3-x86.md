<!-- SPDX-License-Identifier: MIT -->
# squatch-dsp cabinet benchmark, long responses

x86_64, g++ (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0,
`-O3 -fno-exceptions -fno-rtti`. `make bench-long` writes this file.

Conditions (added by hand from the run's own log; the long table did not print them when it ran): AMD Ryzen 9
9950X, Ubuntu 24.04, kernel 7.2.6-76070206-generic, g++ (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0, flags
`-std=c++17 -fno-exceptions -fno-rtti -Wall -Wextra -Wpedantic -Wshadow -Werror -Iinclude
-Iblocks/tuner/include -O3`, governor powersave (balance_performance EPP, unchanged), run 3 of 3, unpinned, on
a shared x86 server. Start: 2026-10-20T15:08:29Z gov=powersave load=3.44 2.64 2.30 gpu=100 %, 15074 MiB
top=python:100.0% python3:45.5% kworker+:9.1% . End: 2026-10-20T15:09:35Z gov=powersave load=3.19 2.74 2.36
gpu=0 %, 2 MiB top=grep:16.7% kswapd0:8.3% kworker+:8.3% .

The NAMBench method as in bench.md (see THIRD_PARTY_NOTICES.md), with passes of 2 s of audio and 6
passes for the block percentile. core_percent is the share of one core; p99 and max are one block's
time as a percent of its own deadline (above 100% a callback was late).

| engine | taps | size | core_percent | block p99 | block max | spread | attempts |
|---|---:|---:|---:|---:|---:|---:|---:|
| old: partition 64 | 4096 | 32 | 0.191% | 0.350% | 1.559% | 0.24% | 1 |
| old: partition 64 | 4096 | 128 | 0.190% | 0.222% | 0.791% | 0.19% | 1 |
| auto (long plan above 16384) | 4096 | 32 | 0.191% | 0.351% | 1.110% | 0.37% | 1 |
| auto (long plan above 16384) | 4096 | 128 | 0.190% | 0.206% | 0.469% | 0.33% | 1 |
| old: partition 64 | 16384 | 32 | 0.459% | 0.896% | 1.823% | 0.15% | 1 |
| old: partition 64 | 16384 | 128 | 0.458% | 0.507% | 0.729% | 0.14% | 1 |
| auto (long plan above 16384) | 16384 | 32 | 0.459% | 0.894% | 4.452% | 0.43% | 1 |
| auto (long plan above 16384) | 16384 | 128 | 0.457% | 0.542% | 1.165% | 0.64% | 1 |
| old: partition 64 | 48000 | 32 | 1.159% | 2.397% | 7.434% | 0.98% | 1 |
| old: partition 64 | 48000 | 128 | 1.156% | 1.256% | 1.586% | 0.31% | 1 |
| auto (long plan above 16384) | 48000 | 32 | 0.361% | 1.356% | 2.264% | 0.41% | 1 |
| auto (long plan above 16384) | 48000 | 128 | 0.359% | 0.935% | 1.037% | 0.45% | 1 |
| old: partition 64 | 240000 | 32 | 5.725% | 12.248% | 19.271% | 0.53% | 1 |
| old: partition 64 | 240000 | 128 | 5.739% | 6.296% | 11.580% | 0.57% | 2 |
| auto (long plan above 16384) | 240000 | 32 | 0.433% | 1.688% | 3.384% | 0.24% | 1 |
| auto (long plan above 16384) | 240000 | 128 | 0.431% | 1.441% | 1.942% | 0.59% | 1 |
