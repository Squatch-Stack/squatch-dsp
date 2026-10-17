# squatch-dsp on the Daisy Seed3

Firmware that runs squatch-dsp blocks, and a Neural Amp Modeler (NAM) A2-Lite model, on an Electrosmith Daisy Seed3
(STM32H750, Cortex-M7 at 480 MHz, 128 KB internal flash). It is a measurement platform first: every image reads the
chip's cycle counter around the work it does and reports the result over USB serial, so a claim about cost is a number
read off the board.

The measured results, the method and how to repeat them are in
[docs/benchmarks/daisy-seed3.md](../../docs/benchmarks/daisy-seed3.md). Licences of what the firmware uses or was generated
from are in [THIRD_PARTY.md](THIRD_PARTY.md).

## What you need

- `arm-none-eabi-gcc`. The numbers in the benchmark note were measured with the Arm GNU Toolchain 15.3.Rel1 (GCC 15.3.1).
  The flash images are close to the 128 KB limit (the amp chain is 98% full), so another compiler version can overflow the
  link; CI pins 15.3.Rel1.
- `make`, `git`, `curl`, `python3`. `numpy` and `clang++` for the accuracy tools. `dfu-util` to flash.
- A Daisy Seed3 and a USB cable, only for flashing and measuring. Building and the host tests need no board.

## Build

```sh
hosts/daisy/fetch-libdaisy        # once: libDaisy at a pinned commit, with our two patches, into hosts/daisy/vendor/
make -C hosts/daisy -j4           # every image, their sizes (build/daisy/SIZES.md) and the heap check
```

`fetch-libdaisy` does not vendor libDaisy. It clones one commit (`facb66c7`, libDaisy master at the Seed3 DevKits merge,
2026-09-24) and the four submodules the build needs at the commits that commit records, applies the two patches in
`patches/`, and writes a stamp holding the commit and the SHA-256 of each patch. `make` re-checks the stamp and refuses to
build if the checkout is anything else. Nothing else in the build downloads anything.

Images are named `seed3-<variant>.bin` in `build/daisy/bin/`. To build one: `make -C hosts/daisy ../../build/daisy/bin/seed3-nam-chain-b32.bin`.

## The images

A variant name is `PROJECT-OPTION-...`; `bNN` is the block size in samples (32 by default, 48 kHz).

| image | what it does |
|---|---|
| `passthrough-bNN` | AUDIO IN 1 and 2 to AUDIO OUT 1 and 2, unchanged. The first sound on a new board and the cost of doing nothing. `passthrough-crash-b32` faults on purpose after 3 s, to check that a crashed image lands in the ROM bootloader. |
| `chain-bNN` | AUDIO IN 1 through the gate, the drive and the limiter to both outputs: three squatch-dsp blocks, with the cost of each per block. Options: `hot` (a +24 dB signal), `gen` (a built-in test signal instead of the input), `laNN` (limiter look-ahead in samples), `limonly`, `burnNN` (a stage that busy-waits NN% of the budget). |
| `latency-bNN` | The Seed measures its own round trip: it writes a click to AUDIO OUT 1, you wire OUT 1 to IN 1, and it finds the click coming back. `latency-lim-...` and `latency-chain-...` put the limiter, or gate + drive + limiter, in the path. |
| `nam-b32` | The A2-Lite model alone: a built-in test input through the model to both outputs. |
| `nam-chain-b32` | Built-in input, gate, drive, A2-Lite, limiter: the whole chain on one core. This is the image the benchmark note calls the chain. |
| `nam-parity-b32` | The model alone fed fixed noise; after the model has warmed up it sends 4,800 input and output samples over serial as float bits, for `tools/nam_check.py`. |
| `nam-bench-b32-...` | Built by `tools/nam_bench.py`: runs the model from the main loop with no audio, times 150 blocks, and records the 4,800 samples. Used to compare kernel variants. |
| `notegen-b32` | A known note on AUDIO OUT 2, stepping through the open strings, with its name, exact frequency and cents printed. A signal source for testing a hardware tuner. |

`make` builds the usual set; `EXTRA_VARIANTS="name ..."` adds more. The block size, `hot`, `gen` and the other options are
compile-time definitions (`-DSQUATCH_BLOCK=32` and so on); the Makefile's header comment lists them.

The A2-Lite images link a libDaisy built `-Os` (`vendor/libDaisy/build-os`), because at `-O2` they do not fit the
128 KB flash. The kernel itself is compiled `-O3`, which it needs (see the benchmark note).

## Flashing over DFU

Nothing is flashed by building. To put an image on a board:

1. Put the Seed3 in its ROM bootloader once: hold BOOT, tap RESET, release BOOT. It then shows up as USB `0483:df11`.
2. `SEED3_SERIAL=<serial> hosts/daisy/tools/flash.py build/daisy/bin/seed3-chain-b32.bin` writes it with
   `dfu-util -a 0 -s 0x08000000:leave -D IMAGE` and waits for the image's banner on the serial port.
   `--backup DIR` reads the old 128 KB flash into `DIR` first, with its SHA-256.

After that first press, every image here answers the line `dfu` on its USB serial port by resetting into the ROM
bootloader, so `flash.py` can reflash without touching the board. If an image crashes, a fault handler also sends it to the
bootloader. An image whose audio callback takes longer than the block starves the main loop and cannot be told to `dfu`;
`nam-b32` and `nam-chain-b32` therefore measure one block's cost before starting audio and refuse to start it above 90% of
the budget.

`SEED3_SERIAL` is the board's USB serial number: with only the Seed plugged in, `ls /dev/cu.usbmodem*` shows
`/dev/cu.usbmodem<serial><digit>`; give the part between. It is never guessed, because other USB boards use the same device
names and `flash.py` writes to whatever it finds. `dfu-util` is pointed only at `0483:df11`, and it refuses to act if more
than one such device is present. `flash.py` is written for macOS; on Linux the port is `/dev/ttyACM*` and the script needs a
small change.

## The serial output

Each image prints one banner and then a few lines a second on the USB serial port (a USB CDC port, so any baud rate works). The lines below are from a run of `nam-chain-b32` on an earlier tree whose limiter was cheaper (the limiter figure differs from this tree's); they show the shape of the output, not this release's numbers.

```
# seed3-nam-chain-b32 block=32 rate=48000 cpu=480000000Hz budget=320000 cycles/block board=seed3 libDaisy=facb66c7
# nam: A2-Lite 1871 weights, receptive field 6347, state 114048 bytes, gate drive nam limiter, latency 88 samples ready
w=5 blocks=1500 late=0 over=0 stretched=0 in=-9.0 out=-21.3 dBFS usb_ungate=1 usb_dropped=2
w=5 cpu 57.70/58.23/58.23 period 99.97/100.01/100.01 (% of block: mean/p99/worst)
w=5 gate 1.09/1.12/1.12 drive 5.92/6.25/6.28 nam 48.13/48.37/48.37 limiter 2.36/2.49/2.49
```

Before it starts audio, the nam images also print `# warm-up: N cycles/block of 320000 budget (P%) -> starting audio`.

- `w=N` is the one-second window. `blocks` is the number of audio callbacks in it (1,500 at block 32); `late` counts
  callbacks that started more than 1.5 block periods after the previous one and `over` counts callbacks whose own work took
  longer than a block period. Both must stay 0.
- `cpu mean/p99/worst` is the whole callback's cost as a share of the block's budget (320,000 cycles at block 32). The 99th
  percentile comes from a 256-bin histogram, so it is exact to 1/128 of the budget; the worst is exact.
- `period` is the time between callbacks, as a share of the block. It sits at 100% when the audio clock is steady.
- one line per stage (`gate`, `drive`, `nam`, `limiter`) with the same three numbers.
- `usb_dropped` counts serial lines the board dropped because the USB buffer was full; it is a logging artefact, not audio.
- the latency image prints `click N: lag=81.33 samples (1.69 ms) ...` and, every 16 clicks, a `LATENCY` summary line.
- the parity image prints `DUMP begin` ... `D <start> <in:out> x 8` lines ... `DUMP end`.
- the bench image prints `# bench: block=32 mean=... min=... max=... warm=... blocks=150 budget=320000 pct=...` and
  `# stack: region=... used_hw=... never_touched=...`.

## Reproducing the benchmark

See the benchmark note's "How to reproduce" section for the whole procedure. In short:

```sh
hosts/daisy/tools/fetch-nam-model                     # NAM Core's A2.nam at a pinned commit, sha256 checked
export SEED3_SERIAL=<your board's USB serial>
hosts/daisy/tools/nam_bench.py --selftest --only 'base,fused+dtcm,fused+dtcm+slideh'
```

`nam_bench.py` builds each kernel variant, flashes it, reads the board's cycle counts and recording, scores the recording
against a float64 reference (`tools/a2_reference.py`) fed the board's own input bits, and writes `results.csv` and
`results.md` under `build/daisy/nam-bench/`. `--selftest` adds a deliberately wrong variant that must be rejected.

## Host tests

`make test` at the repository root builds `hosts/daisy/tests/*.cpp` for the host with the sanitizers: the cycle statistics,
the click detector, the drive block, and the A2-Lite kernel (bit-identical across block sizes, and checked against the plain
kernel). `python3 hosts/daisy/tests/test_tools.py` tests the Python tools' guards. Neither needs a board.

## Files

| path | what |
|---|---|
| `seed3-*/main.cpp` | one firmware per project: passthrough, chain, latency, nam, notegen |
| `common/` | board support shared by the images: cycle counter, serial, window statistics, click detector |
| `nam/` | the A2-Lite kernel (`a2lite.hpp`) and the model's weights (`a2lite_weights.hpp`, generated) |
| `faust/` | the drive block and its Faust-generated core (see THIRD_PARTY.md for the licence of the generated file) |
| `tools/` | flashing, benchmark and accuracy tools, the heap check (fails an image whose audio callback can reach `malloc`), the size report |
| `patches/` | our two patches to libDaisy and why they exist (THIRD_PARTY.md) |
| `tests/` | host-side tests |
