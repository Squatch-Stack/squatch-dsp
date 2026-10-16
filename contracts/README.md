<!-- SPDX-License-Identifier: MIT -->
# Control contracts

One file per block, `<key>.params.json`, in the `cc.squatch.control/1` profile that squatch-skin
defines (`schema/control.schema.json` there). Each file is the only place the block's parameter
ids, keys, ranges, defaults, tapers, steps, labels, roles, meters and identity are written.
Everything else is generated from it:

| Output | By | Used by |
|---|---|---|
| `include/squatch/dsp/contracts/<key>.hpp`: the `Param` enum, the `ParamDesc` table, `BlockInfo` | `tools/build-contracts` | the block (it inherits `contracts::<Block>Contract`) and every C++ host |
| `hosts/clap/generated/<key>.cpp`, `plugins.cmake` | `tools/build-contracts` | the CLAP, VST3 and AUv2 plugins |
| LV2 Turtle, WAM 2.0, OSCQuery, MIDI-CI | squatch-skin's own emitters, run by `tools/build-contracts --formats DIR` | the Pi (PiPedal), the browser rig, networked devices, USB-MIDI |

```sh
tools/build-contracts                        # rewrite the generated files
tools/build-contracts --check                # fail if any is stale (make ci runs this)
tools/build-contracts --formats build/contracts   # also LV2/WAM/OSCQuery/MIDI-CI, via squatch-skin
```

The generator checks everything squatch-skin's `checkDescriptor()` checks (u32 ids never reused,
LV2-symbol keys, units, tapers, at most one bypass), plus what a block needs: a C++ class and
header, a semantic version, a state version, an AU subtype, CLAP features, exactly one bypass and
only value roles (`param`, `bypass`, `meter`, `trigger`; footswitch roles belong to devices, not
blocks).

A `trigger` is a 0..1 toggle whose rising edge starts something in the block (a learn). It is
settable and saved like a parameter, but a host passes it on only as a change it was sent, never
when it restores a state, so loading a preset fires nothing. squatch-skin has no trigger role, so
`--formats` hands it to the skin's emitters as a `param` with `"trigger": true` (the LV2 emitter's
convention).

Contracts that `tools/private-paths` lists stay in the development tree: their plugins go to
`hosts/clap/generated/plugins-private.cmake`, which is never exported, instead of `plugins.cmake`.

## Fields squatch-dsp adds to `product`

`version` (semantic, of the sound and parameters), `stateVersion` (goes up when a saved value
would be read differently), `block` and `header` (the C++ class), `description`, `features`
(CLAP feature strings), `auSubtype` (four characters; the AU manufacturer is `SqSt`), and for
blocks that read a file, `files` (the cabinet's impulse response: a path in the saved state,
never a parameter). squatch-skin ignores fields it does not know.

## Ids, order and keys

- `id` is the stable, never-reused number a host automates (the CLAP param id). Parameters use
  small numbers, meters 100 and up.
- `key` is what presets and states store, so a state reads the same on every target.
- In C++ the enum is dense: the settable parameters (triggers among them) in file order, then the
  bypass, then the meters. `kParamCount` counts the first group; `kCount` all of them.
- What a block learns (a noise profile) is not a parameter: it is the block's learned state
  (Block v2 `saveState()`/`loadState()`), carries its own format version, and a plugin saves it
  beside the parameters. `stateVersion` still covers how the saved parameters are read.

## Conventions settled on 2026-09-25

The C++ blocks and the browser rig's JavaScript (`squatch-rig/web/worklets/dsp/`) disagreed, so a
preset did not carry between them. The rule used: **ranges are the union of both sides**, so every
existing preset from either side is representable without clamping; **sign conventions follow the
more common one** in hosts and hardware; **defaults are the C++ block's** (it carries the tests and
the PiPedal heritage) unless that default was unusable. The golden renders did not change.

### Gate (`gate.params.json`)

| Control | C++ before | JS before | Settled |
|---|---|---|---|
| threshold | -120..0, default -90 | -90..-10, default -58 | **-120..0 dB, default -60** (-90 leaves the gate open on any real DI; -60 is what the Pi rig runs) |
| hysteresis | **-30..0**, default -6: close at threshold **+** hysteresis | 0..20, default 6: close at threshold **-** hysteresis | **0..30 dB, default 6, a positive magnitude: the gate closes below threshold - hysteresis** |
| range | -60..-6, default -60 | -90..0, default -70 | **-90..0 dB, default -60**; the ramp rate treats the range as at least 6 dB, so 0 dB still moves |
| attack | 1..500 ms, default 1 | 0.1..50 ms (log), default 1 | **0.1..500 ms, log taper, default 1** |
| hold | 1..100 ms, default 100 | 0..500 ms, default 40 | **0..500 ms, default 100** |
| release | 10..1000 ms, default 330 | 5..1000 ms (log), default 90 | **5..1000 ms, log taper, default 330** |
| bypass, meters | none | none | bypass (id 7); meters `reduction` (dB) and `open` |

### Limiter (`limiter.params.json`)

| Control | C++ before | JS before | Settled |
|---|---|---|---|
| drive | none | key `gain`, 0..18 dB, default 0 | **key `drive`, 0..18 dB, default 0, smoothed 20 ms**, before the limiter (new in C++) |
| ceiling | -24..0, default -1 | -12..0, default -1 | **-24..0 dBTP, default -1** (true peak) |
| release | 10..1000 ms, default 50 | 10..1000 ms (log), default 120 | **10..1000 ms, log taper, default 50** |
| output | none | none | **-24..0 dB, default 0, smoothed 20 ms**, after the final clamp: it only attenuates, so the output never exceeds the ceiling (new on both sides) |
| bypass, meters | none | `grDb` inside the worklet | bypass (id 5); meter `reduction` (dB, the most in the last buffer) |

### What the JavaScript side must change (squatch-rig, not edited here)

1. **Gate** (`web/worklets/dsp/gate.js`): widen threshold to -120..0 (default -60), hysteresis to
   0..30 (sign already right), range to -90..0 (default -60), attack to 0.1..500, hold to 0..500
   (default 100), release to 5..1000 (default 330). The JS detector still differs from the C++
   one (a 20 ms peak envelope and one-pole gain moves against the C++ instantaneous peak and dB-linear
   ramps), so identical settings will still sound slightly different until the rig runs the WASM
   block.
2. **Limiter** (`web/worklets/dsp/limiter.js`): rename the key `gain` to `drive` (and migrate saved
   `.rig.json` presets), widen ceiling to -24..0, release default 120 -> 50, add `output` (-24..0 dB
   after the ceiling). Its lookahead is 2 ms against the C++ 1.5 ms plus the true-peak
   interpolator, so latency differs (the C++ reports 88 samples at 48 kHz: 72 of lookahead and 16 of interpolator).
3. Generate both maps from these contracts, as `tools/build-bloom-contract.mjs` already does for
   Bloom and Trail OD, so they cannot drift again.
