<!-- SPDX-License-Identifier: MIT -->
# Third-party notices

squatch-dsp's own code is MIT (see LICENSE). The files below are adapted from third-party code under
the MIT licence, and keep the copyright notice it requires.

## pedal-science, by Rik Hemsley

Source repositories (links pinned to the commit adapted from):

- [pedal-science/namchain](https://github.com/pedal-science/namchain) at e208bb869069b15a7331b452e03b7dc88726ffee
- [rikkus/OptimisationWorkOnNeuralAmpModelerCore](https://github.com/rikkus/OptimisationWorkOnNeuralAmpModelerCore)
  at 010a0e53bee7ea8867d8c65917e5bdea3babf60f (a fork of NeuralAmpModelerCore, MIT, below)
- [pedal-science/NAMBench](https://github.com/pedal-science/NAMBench) at 0a5ef0086852294f8450606e78c44760e1055e35

| What here | Adapted from |
|---|---|
| `include/squatch/dsp/crossfade.hpp`, `src/crossfade.cpp`, the cases in `tests/test_crossfade.cpp` (the correlation is a running estimate here, not the first block's: ours) | [crossfade.h](https://github.com/pedal-science/namchain/blob/e208bb869069b15a7331b452e03b7dc88726ffee/include/namchain/rt/crossfade.h), [crossfade.cpp](https://github.com/pedal-science/namchain/blob/e208bb869069b15a7331b452e03b7dc88726ffee/src/rt/crossfade.cpp), [test_crossfade.cpp](https://github.com/pedal-science/namchain/blob/e208bb869069b15a7331b452e03b7dc88726ffee/tests/test_crossfade.cpp) |
| `tests/glitch.hpp`, `tests/test_glitch.cpp`; applied in `tests/test_bypass.cpp`, `tests/test_model_swap.cpp`, `tests/test_cabinet.cpp`, `tests/test_tuner.cpp` | [glitch.h](https://github.com/pedal-science/namchain/blob/e208bb869069b15a7331b452e03b7dc88726ffee/include/namchain/glitch.h), [glitch.cpp](https://github.com/pedal-science/namchain/blob/e208bb869069b15a7331b452e03b7dc88726ffee/src/glitch/glitch.cpp), [test_glitch.cpp](https://github.com/pedal-science/namchain/blob/e208bb869069b15a7331b452e03b7dc88726ffee/tests/test_glitch.cpp) (the four rules and their defaults; rewritten, not copied) |
| `include/squatch/dsp/blocks/model_swap.hpp`, `src/model_swap.cpp`, `include/squatch/dsp/blocks/bypass.hpp` and `src/bypass.cpp` (the warm-up before a fade), the cases in `tests/test_model_swap.cpp` | [transitions.md](https://github.com/pedal-science/namchain/blob/e208bb869069b15a7331b452e03b7dc88726ffee/docs/transitions.md), [lookback.h](https://github.com/pedal-science/namchain/blob/e208bb869069b15a7331b452e03b7dc88726ffee/include/namchain/lookback.h), [crossfade.h](https://github.com/pedal-science/namchain/blob/e208bb869069b15a7331b452e03b7dc88726ffee/include/namchain/rt/crossfade.h) (the plan: warm over the lookback, then crossfade; no code copied) |
| `include/squatch/dsp/tail_convolver.hpp`, `src/tail_convolver.cpp`, the long plan in `src/cabinet.cpp`, the stepped transform in `src/fft.cpp` | [linear.cpp](https://github.com/rikkus/OptimisationWorkOnNeuralAmpModelerCore/blob/010a0e53bee7ea8867d8c65917e5bdea3babf60f/NAM/linear.cpp), [linear.h](https://github.com/rikkus/OptimisationWorkOnNeuralAmpModelerCore/blob/010a0e53bee7ea8867d8c65917e5bdea3babf60f/NAM/linear.h), [linear.rst](https://github.com/rikkus/OptimisationWorkOnNeuralAmpModelerCore/blob/010a0e53bee7ea8867d8c65917e5bdea3babf60f/docs/linear.rst) (the partition table and the tail tier's scheduling; algorithm only, no code copied) |
| `bench/protocol.hpp`, the second table of `bench/bench.cpp`, `tests/test_bench_protocol.cpp` | [nb_protocol.h](https://github.com/pedal-science/NAMBench/blob/0a5ef0086852294f8450606e78c44760e1055e35/Tools/nb_protocol.h), [nam_ir_benchmark.cpp](https://github.com/pedal-science/NAMBench/blob/0a5ef0086852294f8450606e78c44760e1055e35/Tools/nam_ir_benchmark.cpp) |

```text
MIT License

Copyright (c) 2026 Rik Hemsley

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## NeuralAmpModelerCore, by Steven Atkinson and contributors

The long-response convolver above is derived from the `Linear` model in a fork of NeuralAmpModelerCore,
which carries this notice (the fork's own changes are Rik Hemsley's, under the licence above):

```text
MIT License

Copyright (c) 2023 Steven Atkinson

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```
