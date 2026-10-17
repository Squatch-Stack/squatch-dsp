# Daisy Seed3: a Neural Amp Modeler on one Cortex-M7

Measured on 2026-10-16 and 2026-10-17. Plain summary: the A2-Lite model from NAM Core's example A2 runs in real time on a
single Daisy Seed3 core, using **46.5% of the chip** by itself and **66.4% on average (69.3% in the worst second)** inside a
chain of gate, drive, amp and limiter, with no late or overrunning audio blocks. Everything marked **measured** was read off the
board's cycle counter or from the linker; **estimate** and **inference** are marked where they appear. The firmware is in
[hosts/daisy](../../hosts/daisy/README.md) (MIT), and this note says how to repeat every measurement.

## Contents

1. [Board and settings](#1-board-and-settings)
2. [The model](#2-the-model)
3. [How it was measured](#3-how-it-was-measured)
4. [Result: the amp alone](#4-result-the-amp-alone)
5. [Every variant we measured](#5-every-variant-we-measured)
6. [The whole chain, with audio](#6-the-whole-chain-with-audio)
7. [Memory](#7-memory)
8. [Round-trip latency](#8-round-trip-latency)
9. [What did not help](#9-what-did-not-help)
10. [What is untested](#10-what-is-untested)
11. [How to reproduce it](#11-how-to-reproduce-it)

## 1. Board and settings

| | |
|---|---|
| board | Electrosmith Daisy Seed3 (STM32H750IB, Cortex-M7 with FPU, 128 KB internal flash, 128 KB DTCM, 512 KB AXI SRAM) |
| clock | 480 MHz core |
| sample rate | 48 kHz |
| block size | 32 samples (also 16, 48 and 64 for the plain chain and latency images) |
| budget | one block lasts 320,000 cycles at 32 samples: 1,500 blocks a second |
| firmware | squatch-dsp `hosts/daisy`, libDaisy `facb66c7` plus two patches, built `-O3` (libDaisy `-Os` for the amp images) |
| compiler | Arm GNU Toolchain 15.3.Rel1, GCC 15.3.1 |
| samples | 32-bit float; the amp is float32 end to end, no integer kernel |

"% of budget" below is the cycles a piece of work takes divided by 320,000. At 100% the work takes the whole block and audio
breaks.

## 2. The model

The amp is the **A2-Lite** submodel of `example_models/A2.nam` in NeuralAmpModelerCore (MIT, Steven Atkinson;
<https://github.com/sdatkinson/NeuralAmpModelerCore>, commit `0b3d3c97`, file sha256 `2d2d7445...fcf4ca`). That file is a
"slimmable" container; the smaller submodel is a WaveNet with 3 channels and 23 dilated layers.

- 1,871 weights (7.5 KB in flash as float literals), receptive field 6,347 samples.
- 1,731 multiply-adds per sample, so 55,392 per 32-sample block.
- It is NAM Core's own example model; its MIT notice is in [hosts/daisy/THIRD_PARTY.md](../../hosts/daisy/THIRD_PARTY.md).
- The kernel (`hosts/daisy/nam/a2lite.hpp`) is our own code for this chip: fixed sizes, no heap, no libm, weights and state in
  static memory. `tools/nam_to_header.py` regenerates the weights header from the pinned model; CI checks that it matches.

## 3. How it was measured

All timings come from the Cortex-M7's cycle counter (DWT `CYCCNT`, a 32-bit counter running at the core clock), read just
before and just after the work. No external instrument, no estimate from a datasheet.

- **Warm-up.** The model starts from silence and its biases settle over the receptive field, so before anything is
  recorded the board runs two receptive fields of silence through it (397 blocks). The same warm-up is how the images refuse to
  start audio if one block costs more than 90% of the budget.
- **150 steady-state blocks.** After warm-up the board feeds the model a fixed pseudo-random noise (a small linear
  congruential generator, amplitude 0.3) one 32-sample block at a time from the main loop, with no audio running, and records the
  cycles each of 150 blocks took (150 x 32 = 4,800 samples). Reported: mean, minimum and maximum. Running with no audio means a
  kernel that is over budget cannot starve the serial port.
- **Parity recording.** The same 4,800 input samples and the 4,800 output samples are sent over USB serial as raw float bits.
  The Mac then runs NAM's network in 64-bit floats (`hosts/daisy/tools/a2_reference.py`, straight from the `.nam` weights,
  fed the board's exact input bits) and scores the board's output as **ESR**, the error-to-signal ratio:
  `10 log10(sum of squared error / sum of squared reference)` in dB. More negative is better. The pipeline's gate is
  -100 dB.
- **Bit-exactness.** For variants that claim not to change the arithmetic, the pipeline also demands that all 4,800 output
  samples equal the plain kernel's output on the board, bit for bit.
- **Negative control.** `nam_bench.py --selftest` adds `xbreak`, a variant with the output scale 1% off. It must be rejected. In
  every sweep it was (ESR -40.0 dB against the -100 dB gate, 0 of 4,800 samples equal), so a variant passing the gate is
  evidence rather than a gate that passes everything.
- **With audio.** The chain images run real audio callbacks on the board's audio clock and report, once a second, mean /
  99th percentile / worst callback cost as a share of the block, and counts of late and overrunning callbacks. Windows are
  one second; the first few are dropped.
- **Stack.** The unused DTCM below the stack is painted with `0xA5A5A5A5` at the top of `main`, and the lowest word that no longer
  holds the paint is read once a second. Not counted: the few hundred bytes the startup code uses before `main`.
- **One board, one compiler.** Everything was measured on one Seed3 with GCC 15.3.1. We have not tried a second board or
  compiler.

## 4. Result: the amp alone

**Measured** on the board, final code, 150 blocks of 32 (2026-10-17):

| | cycles per 32-sample block | % of budget | cycles per sample | cycles per multiply-add |
|---|---|---|---|---|
| plain kernel we started from | 329,950 | 103.1% | 10,311 | 5.96 |
| **final kernel (fused tiles, state in DTCM, head over a sliding window)** | **148,656** | **46.5%** | **4,646** | **2.68** |

- Evidence: `docs/benchmarks/data/daisy-seed3/nam-bench-final-sweep.csv` (the board's cycle counts, ESR and sample counts), and
  `hosts/daisy/tools/nam_check.py` for the board-against-Mac comparison.
- Minimum 146,336 and maximum 154,790 cycles over the 150 blocks.
- Accuracy: ESR **-123.9 dB** against the float64 reference, largest sample error 5.4e-08. The final kernel's output is
  **bit-identical to the plain kernel's on the board: 4800 of 4800 samples**.
- The same kernel built for the Mac is bit-identical to the plain kernel at block sizes 7, 16, 28, 32, 33, 48, 64 and 100
  (measured on the Mac). Board against the Mac build (column `note` of `docs/benchmarks/data/daisy-seed3/nam-bench-final-sweep.csv`): 234 of 4,800 samples bit-identical, the rest differ in the last bits
  because the two compilers fuse multiply-adds differently; both have the same ESR against the reference.
- The plain kernel, at 103%, does not run in real time. The kernel is 2.2 times faster.

What the kernel does differently (from the code, and the counters in section 5):

1. It computes all three output channels of a tile of 4 frames together, with 12 accumulators in registers, so each loaded
   history value is used three times; the weights are repacked `[input][tap][output]`.
2. The activation is `max(z, 0.01 z)`, the same value for every finite input and free of branches.
3. The residual stream is stored straight into the next layer's ring buffer: no copy per layer.
4. The whole model object (rings, scratch, weights copy) lives in DTCM, the chip's zero-wait-state memory.
5. The head (a 16-tap convolution over three channels) works on 4 frames and two taps per pass.

Why a tile of 3 outputs by 4 frames: on this core a floating-point multiply-add does not overlap with a load. Micro-benchmarks
on the board (**measured**, 1,000 passes with data in DTCM): 12 independent multiply-adds cost 12 cycles; 12 multiply-adds
plus 7 loads cost 19; 12 loads alone cost 8. So a pass costs the multiply-adds plus about one cycle per loaded word, and the
way to go faster is fewer loads per multiply-add, not a different layout.

## 5. Every variant we measured

All rows are **measured** on the board with the pipeline in section 3. A kernel built from a different tree has a slightly
different code layout, and the same source compiled to different cycle counts in different trees (the plain kernel took
351,737 cycles in the first experimental tree and 329,950 in the clean one), so **compare rows within a table, not across
tables**. "The plain kernel" is the first, straightforward float32 kernel; a variant is "bit-identical" when all 4,800 recorded
output samples equal its output on the board. The `results.csv` files of the two 2026-10-17 sweeps are in
[data/daisy-seed3](data/daisy-seed3); the earlier sweeps' CSV files were not kept, and their figures (5.3, 5.4) are the pipeline's tables as recorded at the time.

### 5.1 Final code, one sweep (2026-10-17)

Evidence: `docs/benchmarks/data/daisy-seed3/nam-bench-final-sweep.csv`; command: `hosts/daisy/tools/nam_bench.py --selftest`.

| variant | what | cycles/block (min, max) | % budget | cycles/MAC | ESR dB | kept |
|---|---|---|---|---|---|---|
| base | plain kernel, rings and state in AXI SRAM | 329,950 (min 325,200, max 337,014) | 103.1 | 5.96 | -123.9 | baseline |
| fused+dtcm | fused tiles, state in DTCM | 154,393 (min 152,618, max 159,980) | 48.2 | 2.79 | -123.9 | yes |
| **fused+dtcm+slideh** | the same, head over a sliding window | **148,656** (min 146,336, max 154,790) | **46.5** | **2.68** | -123.9 | **shipping** |
| break | negative control: output scale 1% off | 329,944 | 103.1 | 5.96 | -40.0 | REJECTED, as it must be |

Output against the plain kernel's, bit for bit, on the board (column `exact_vs_base` in `docs/benchmarks/data/daisy-seed3/nam-bench-final-sweep.csv`): `fused+dtcm` and `fused+dtcm+slideh` match in all 4800 samples; `break` in none, which is why it is rejected.

### 5.2 The step that tried the NAM Core layout and the sliding windows (2026-10-17, one build before the final code)

Evidence: `docs/benchmarks/data/daisy-seed3/nam-bench-first-sweep.csv`; the same command with the extra variants.

| variant | what | cycles/block | % budget | ESR dB | kept |
|---|---|---|---|---|---|
| base | plain kernel | 329,980 | 103.1 | -123.9 | baseline |
| fused+dtcm | fused tiles, state in DTCM | 154,912 | 48.4 | -123.9 | reference |
| slidel | sliding window on the four dilation-1 layers | 154,555 | 48.3 | -123.9 | no: 0.23% faster, 1.9 KB more flash |
| slideh | sliding window on the head | 151,010 | 47.2 | -123.9 | **yes: 2.5% faster** |
| slide | both | 150,934 | 47.2 | -123.9 | no: 76 cycles (0.05%) better than slideh alone |
| a2fast | NAM Core's `a2_fast` layout, state in DTCM | 208,343 | 65.1 | -123.3 | **no: 34% slower** |
| a2fast-axi | the same, state in AXI SRAM | 214,332 | 67.0 | -123.3 | no |
| break | negative control | 330,044 | 103.1 | -40.0 | REJECTED, as it must be |

Output against the plain kernel's, bit for bit, on the board (column `exact_vs_base` in `docs/benchmarks/data/daisy-seed3/nam-bench-first-sweep.csv`): `fused+dtcm`, `slidel`, `slideh` and `slide` match in all 4800 samples; `a2fast` and `a2fast-axi` in only 175 (the sum order differs, the ESR is still -123.3 dB); `break` in none.

The code for `slidel` and `a2fast` is not in this release: neither earned its place. They are described in section 9.

### 5.3 The sweep of the code before that (2026-10-16)

| variant | what | cycles/block | % budget | ESR dB | kept |
|---|---|---|---|---|---|
| base | plain kernel | 329,947 | 103.1 | -123.9 | baseline |
| dtcm | object in DTCM only | 320,987 | 100.3 | -123.9 | no: 2.7% |
| fused | 3 outputs x 4 frames, repacked weights, direct stores | 172,871 | 54.0 | -123.9 | no, not alone |
| **fused+dtcm** | | **154,896** | **48.4** | -123.9 | yes |
| fused+dtcm+lto | and `-flto` | 153,951 | 48.1 | -123.9 | no: 0.6%, and it costs a build-system change |
| fused+dtcm+fast | and `-ffast-math` | 166,803 | 52.1 | -122.5 | no: slower and less exact |
| fused+dtcm+o2 | image compiled `-O2` instead of `-O3` | 671,622 | 209.9 | -123.9 | no: 4.3 times slower |

All of these matched the plain kernel in all 4,800 samples except `fused+dtcm+fast` (`-ffast-math`, 202 of 4,800 equal).

### 5.4 What each step bought, in the first experimental tree (2026-10-16)

| step | cycles/block | note |
|---|---|---|
| plain | 351,737 | |
| + branch-free activation | 316,526 | -10% |
| + fused 3-output tiles, repacked weights | 244,399 | -31% on its own |
| + object in DTCM | 235,017 | -4%: the fused kernel is sensitive to where its memory is, the plain one is not |
| + epilogue constants in locals, frames unrolled | 202,984 | |
| + residual stored straight into the next ring | 163,366 | -20%: no `x` array, no per-layer copy |
| the same, rewritten cleanly (final tree, `fused+dtcm`) | 154,896 | the cleaner code compiled better |
| 8 frames per tile | 164,817 | the compiler spills registers; no gain over 4 |
| 2 frames per tile | 190,094 | |
| tap loop specialised on kernel size | 184,231 | worse: bigger code |
| bias and mixin folded into the start of the sum | 155,945 | -4.5% against 163,366, ESR -124.0 dB; **not adopted**, see section 9 |
| 3 taps per pass, loads first | 197,255 | worse |
| compiler pre-RA scheduler (`-fschedule-insns -fsched-pressure`) | 163,366 | no change |
| image `-O2` / `-Os` instead of `-O3` | 330,476 / 600,792 | the kernel depends on `-O3`'s unrolling and interleaving |

Where the plain kernel's cycles went (**measured** with the DWT's CPI, load-store and folded-instruction counters around each
tap pass): 4.93 cycles per multiply-add inside the tap loops, which were 56% of a block; the rest was activation branches, the
per-layer copy into the rings, and the residual. Moving the plain kernel's memory to DTCM changed it by only 2.5%, so it was
not waiting for memory: it was issuing too many instructions.

## 6. The whole chain, with audio

The `nam-chain-b32` image runs, in one audio callback at 32 samples and 48 kHz: **gate, drive, A2-Lite, limiter**, from a
built-in test input to both outputs. Measured 2026-10-17, final code, audio on for 36 s:

| | result |
|---|---|
| late blocks / overrunning blocks | **0 / 0** |
| whole callback, mean | **66.43% of budget** |
| whole callback, worst one-second window | **69.28%** |
| the A2-Lite stage alone | 47.08% |
| the other three stages together (arithmetic: 66.43 - 47.08) | about 19 points |
| warm-up guard reading before audio starts | 65.7% |

- In the same session, the chain with only the fused kernel (without the sliding head) gave 67.73% mean, 70.11% worst, and
  48.10% for the A2-Lite stage: the head window is worth about 1.3 points of total CPU.
- The three-stage chain without the amp (gate, drive, limiter), measured on this tree's blocks 2026-10-16: 18.7% mean and
  18.9% worst at block 32; 18.6% / 18.9% at 48; 18.5% / 18.8% at 64. About 1.3% is the gate, 5.3% the drive and 12.0% the limiter.
- The built-in test input is a fixed signal, not a guitar. The A2-Lite's cost does not depend on the signal (the kernel has no
  data-dependent branches), and this release's limiter costs the same on any signal.

The chain fits the Seed's internal flash, so no bootloader, QSPI flash or SDRAM is needed. It also has about a third of the CPU
left over (**inference** from the 69.3% worst window: 30.7% free).

## 7. Memory

**Measured** from the linker's own memory report (`make -C hosts/daisy` writes `build/daisy/SIZES.md`):

| image | flash (of 131,072 B) | DTCM (of 131,072 B) |
|---|---|---|
| `nam-b32` (amp alone) | 96,664 B (73.7%) | 124,564 B (95.0%) |
| `nam-parity-b32` | 94,676 B (72.2%) | 124,564 B (95.0%) |
| **`nam-chain-b32`** (the chain) | **128,464 B (98.0%)**, 2,608 B spare | **124,564 B (95.03%)**, 6,508 B left for the stack |

- The A2-Lite images need libDaisy built `-Os` (about 20 KB smaller): at `-O2` the amp alone is 8 KB over the flash and the
  chain 15 KB over.
- **Stack high-water mark, measured.** Amp bench image (model run from the main loop, no audio): 1,912 to 1,944 bytes used of the
  6,508. Chain image with audio on (the interrupt callback shares the stack), 40 one-second windows: **2,896 bytes used,
  3,612 bytes never touched (55%)**.
- Only about 2.6 KB of flash is spare in the chain image. Any further feature would need the Daisy bootloader with QSPI or SRAM
  execution (**estimate** of what it would take: not tried).
- **Which image ran.** Built on a Mac with GCC 15.3.1, `seed3-nam-chain-b32.bin` is 128,464 bytes with sha256
  `15db1229cf0274545a8584c5d5b61067a1cebc5af7e68277a7adfd1dba7f7178`: the image that was on the board for the chain run. The
  The same code built in CI with the same compiler release but its Linux (x86_64) build is 128,456 bytes: 8 bytes different. We
  have not found why. So compare the size and the flash percentage across machines, not the hash.

## 8. Round-trip latency

**Measured**, with AUDIO OUT 1 (pin 18) wired to AUDIO IN 1 (pin 16). The Seed writes a one-sample click and finds it coming back.
Each row is 56 to 57 clicks over 14 s with **zero jitter** (every click gave the same lag to 0.01 sample), peak -8.4 dBFS,
noise about -103 dBFS:

| block | round trip (samples) | round trip (ms) | 2 x block | rest (converters) |
|---|---|---|---|---|
| 16 | 49.33 | 1.028 | 32 | 17.33 |
| 32 | 81.33 | 1.694 | 64 | 17.33 |
| 48 | 113.33 | 2.361 | 96 | 17.33 |
| 64 | 145.33 | 3.028 | 128 | 17.33 |

The lag is `2 x block + 17.33` samples at every block size: two blocks of buffering plus 0.361 ms of converter filter delay.

With effects in the path, block 32, 72-sample limiter look-ahead (the default), **measured**:

| path | round trip |
|---|---|
| limiter only | 169.33 samples = 3.528 ms |
| gate + drive + limiter | 182.16 samples = 3.795 ms |

The limiter adds 88 samples (its look-ahead plus 16 samples for half its true-peak filter); the drive's filters add 12.83.
Those two images are built `-Os` to fit the flash, so their CPU is not comparable with the `-O3` figures.

The round trip with the A2-Lite in the path was **not measured**. **Inference:** the model works on each sample from the
past only, with no look-ahead, so it should add no delay and the chain should round-trip in about 182 samples (3.8 ms) at
block 32. The image prints `latency 88 samples` as the delay its stages declare, which is the limiter's alone.

## 9. What did not help

All **measured** (tables in section 5):

- **NAM Core's `a2_fast` layout, ported to the M7: 208,343 cycles against 154,912, 34% slower** (65.1% of budget against
  48.4%; 214,332 in AXI SRAM). We ported its structure from `NAM/wavenet/a2_fast.cpp`: interleaved three-float columns, one tap at a
  time over the whole block, the partial sums kept in memory between taps. Reading (**inference**): each tap pass loads 3
  history and 3 partial-sum words and stores 3, for 9 multiply-adds, and a multiply-add does not overlap a load on this core, so
  the round trip through memory costs more than holding 12 accumulators in registers. Two deliberate deviations: we kept
  this file's ring lengths (the original's power-of-two rings would need more than the 128 KB of DTCM), and the tap loop is a
  template on 6 or 15 as in the source. It is not bit-identical to the plain kernel (the sum order differs) but passes the
  accuracy gate at -123.3 dB. This says the layout is not the better one **for this core**, not that it is slow elsewhere.
- **A sliding window over the four dilation-1 layers: 0.23% faster** (154,912 to 154,555), for 1.9 KB more flash out of the 3 KB
  left in the chain image. Only 33 of the 156 taps are on those layers. Combined with the head window the gain vanishes
  (150,934 against 151,010). The estimate we had made beforehand was 5% to 8% for sliding windows; the measurement was 2.5% for
  the head and nothing for the rest.
- **`-O2` instead of `-O3` for the image: 2.0 to 4.3 times slower** (330,476 against 163,366 in the first tree; 671,622
  cycles, 209.9% of budget, against 154,896 in the later one), and `-Os` 3.7 times slower (600,792 against 163,366).
  The kernel depends on `-O3`'s unrolling and interleaving; do not lower the optimisation level of the amp image.
- **The inexact fold** (bias and mixin folded into the start of the sum, residual taken from `x + bias`): 4.5% fewer cycles
  (155,945 against 163,366 in that tree, ESR -124.0 dB) but the output differs from the plain kernel in 4,573 of 4,800 samples.
  The error against the float64 reference is unchanged, but we kept the rule that the shipping kernel is bit-identical to the
  plain one, so it was not adopted.
- **`-ffast-math`**: slower (166,803 against 154,896) and less exact (-122.5 dB).
- **`-flto`**: 0.6% faster, not worth the build-system change. **DTCM alone** (without the fused tiles): 2.7%.
- **8 frames per tile**: no gain (164,817 against 163,366), the compiler spills. **2 frames**: 190,094.
- **ITCM** (code in the instruction-tightly-coupled RAM) was not tried: the stock linker script has no section placed there. The
  code runs from the I-cache.
- **CMSIS-DSP routines**: `arm_conv_f32` and `arm_mat_mult_f32` take whole sequences and are scalar float loops on an M7 without
  Helium; none does a 3-in 3-out dilated tap with a ring mirror, and the micro-benchmark says the loads are the cost, which a
  library call does not change. Not used.

## 10. What is untested

- **Estimates that were never measured:** a tile of 8 frames with hand-scheduled `vldm` loads would save about 8%
  (**estimate**; the compiler's version spilled). A 4-tap group on the head would need 33 live floats and spill
  (**estimate**).
- **Block sizes other than 32 for the amp.** Only block 32 was run for the amp images. The kernel is bit-identical across block
  sizes on the Mac, but its CPU at 16, 48 or 64 was not measured on the board.
- **Real playing.** The amp and chain images use a built-in input. Nothing was played through them with a guitar, and no one
  has judged the sound. The accuracy claim is ESR against the float64 reference on noise, not a listening test.
- **Long runs.** The longest chain run was 36 s (40 windows for the stack). Heat and long-term stability were not looked at.
- **One board and one compiler.** Another Seed3, a Seed 2, a different GCC, or a different libDaisy commit may differ,
  especially where the flash is 98% full.
- **The round trip with the amp** (section 8).
- **Splitting the model across two chips** (one chip per half of the layers, over SAI) was considered and not built, because one
  core was enough.
- **Operating systems.** The flashing and measurement tools were written and used on macOS. The firmware build is plain `make`
  and CI builds it on Linux (`.github/workflows/daisy.yml`); the tools have not been run on Linux or Windows.

## 11. How to reproduce it

You need a Daisy Seed3, `arm-none-eabi-gcc` 15.3.Rel1 (other versions may overflow the flash), `dfu-util`, `make`, `git`, `curl`,
`python3` with `numpy`, and `clang++`.

```sh
git clone https://github.com/squatch-stack/squatch-dsp && cd squatch-dsp

hosts/daisy/fetch-libdaisy                   # libDaisy at the pinned commit, with the two patches
make -C hosts/daisy -j4                      # all images; build/daisy/SIZES.md has flash and RAM (section 7)
grep nam-chain-b32 build/daisy/SIZES.md      # about 128,460 bytes, 98.0% of the flash (section 7)

hosts/daisy/tools/fetch-nam-model            # NAM Core's A2.nam, checked against its sha256
export SEED3_SERIAL=<the board's USB serial>  # see hosts/daisy/README.md
```

Put the board in its bootloader once (hold BOOT, tap RESET, release BOOT). Then:

1. **The amp alone (section 4, 5.1)**:
   `hosts/daisy/tools/nam_bench.py --selftest --only 'base,fused+dtcm,fused+dtcm+slideh'`. It builds, flashes and measures each
   variant and the negative control and writes `build/daisy/nam-bench/<time>/results.md` and `.csv`. Expect 148,656 cycles for
   `fused+dtcm+slideh` and `break` to be REJECTED. Your cycles should match to within a fraction of a percent if the compiler is the
   same.
2. **Accuracy on the parity image**: `hosts/daisy/tools/flash.py build/daisy/bin/seed3-nam-parity-b32.bin`, then
   `hosts/daisy/tools/nam_check.py`. It prints the number of bit-identical samples against the same kernel on the Mac, and the ESR
   against the float64 reference (expect about -123.9 dB, largest error about 5.4e-08).
3. **The chain with audio (section 6)**: flash `build/daisy/bin/seed3-nam-chain-b32.bin` and run
   `hosts/daisy/tools/serial_capture.py 40 chain.txt`. Read the `cpu`, `nam` and `blocks=... late=... over=...` lines: `late` and `over`
   must stay 0.
4. **Round trip (section 8)**: wire AUDIO OUT 1 (pin 18) to AUDIO IN 1 (pin 16), flash `seed3-latency-b32.bin`, and read the
   `LATENCY` line.
5. **Stack and memory**: the `# stack:` line from the bench image; `build/daisy/SIZES.md` for the rest.
6. **The weights header** needs no board: `hosts/daisy/tools/nam_to_header.py build/nam-model/A2.nam | diff - hosts/daisy/nam/a2lite_weights.hpp`
   prints nothing.

The host-side tests (`make test`) check the kernel is bit-identical across block sizes and the tools' guards, without a board.
