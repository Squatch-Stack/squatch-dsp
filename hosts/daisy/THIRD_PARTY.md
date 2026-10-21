# Third-party notices for hosts/daisy

The firmware in this folder is MIT, like the rest of squatch-dsp. It uses, or was generated from, the work of others.
Each
notice below is reproduced as its licence requires.

## NeuralAmpModelerCore: the model weights (MIT)

`nam/a2lite_weights.hpp` holds the 1,871 weights of the A2-Lite submodel (3 channels, 23 layers) of the example model
`example_models/A2.nam` in NeuralAmpModelerCore, <https://github.com/sdatkinson/NeuralAmpModelerCore>, commit
`0b3d3c97b0859a3a8c92a8628c4dd89a25eb5842` (sha256 of the file
`2d2d744516dc0197737a2c5001010429692d4cb20d72b08264781de626fcf4ca`).
`tools/fetch-nam-model` downloads that exact file and `tools/nam_to_header.py` regenerates the header from it. The
network
layout in `nam/a2lite.hpp` follows NAM Core's A2 WaveNet; the code is our own. Nothing trained on any other capture is
here.

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

## libDaisy (MIT), fetched, not copied

The firmware links Electrosmith's libDaisy, <https://github.com/electro-smith/libDaisy>. It is not vendored:
`fetch-libdaisy`
clones commit `facb66c76b5482918741695f4268b0185e474644` (master at the Seed3 DevKits merge, 2026-09-24; no tagged
release has
Seed3 support) and its four submodules at the commits that commit records, into `vendor/` (gitignored), and refuses to
build
if anything differs. The libDaisy licence travels with the checkout (`vendor/libDaisy/LICENSE`); its notice is:

```text
The MIT License (MIT)

Copyright (c) 2019 Electrosmith

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

libDaisy's own submodules (ARM CMSIS, STMicroelectronics' CMSIS device files, HAL driver and USB device library) carry
their own licences inside the fetched checkout; none of their files is copied into this repository. A firmware binary
you
build contains parts of them, so keep those licence files with any binary you distribute.

### Our two patches to libDaisy, and why each exists

Both are applied by `fetch-libdaisy` from `patches/`, and both come from upstream pull requests (kept here because
libDaisy
has not released a version with them). Neither changes the Make build this folder uses; they matter to anyone who builds
libDaisy with CMake.

| patch | what | why |
|---|---|---|
| `0001-cmake-define-DATA_IN_D2_SRAM.patch` | libDaisy PR #713 (fixes #712) | The CMake build never defined `DATA_IN_D2_SRAM`, so `SystemInit()` left the D2 SRAM, which holds the audio DMA buffers, switched off. The Make build already defines it. |
| `0002-linker-scripts-drop-malformed-DISCARD.patch` | libDaisy PR #715 (fixes #714) | A malformed `DISCARD` block in the linker scripts named `libc.a`, `libm.a` and `libgcc.a`, which made CMake links load all of newlib. The Make link is byte-identical with or without it. |

## Faust libraries: the generated drive (`faust/drive_faust.hpp`)

`faust/drive_faust.hpp` is the C++ output of the Faust compiler 2.85.9 for an overdrive model that is our own work,
adapted by
`faust/generate`. The licence lines of the Faust libraries it was compiled from were read in that release's
`share/faust`
on 2026-10-17:

| library and functions used | licence line in the file | effect on this header |
|---|---|---|
| `filters.lib`: `highpass`, `lowpass`, `tf1`, `tf1s` | "Copyright (C) 2003-2019 by Julius O. Smith III", "MIT-style STK-4.3 license" | permissive; the copyright and permission notice must stay with copies: it is printed below and in the header of `drive_faust.hpp` |
| `signals.lib`, "jos section": `smooth` (`smoo`) | "Copyright (C) 2003-2017 by Julius O. Smith III ... released under the (MIT-style) STK-4.3 license" | same |
| `maths.lib` (`tanh`, `PI`), `platform.lib` (`SR`), `signals.lib` GRAME section | "Copyright (C) 2003-2016 / 2020 GRAME", "GNU Lesser General Public License ... EXCEPTION TO THE LGPL LICENSE" | the exception lets you "freely choose the license for the resulting compiled code ... no obligation to be LGPL or GPL", so MIT is allowed |

The Faust compiler itself (LGPL 2.1 or later) is not part of this repository, and no Faust architecture file is
included: the
header is the bare generated class, with the Faust UI and metadata members removed.

```text
STK 4.3 licence (Julius O. Smith III), as printed at the end of Faust's filters.lib:

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

Any person wishing to distribute modifications to the Software is asked to send
the modifications to the original developer so that they can be incorporated
into the canonical version.  For software copyrighted by Julius O. Smith III,
email your modifications to him at the address printed in Faust's filters.lib.  This is, however, not a
binding provision of this license.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

The Faust source of the model is not published, so `faust/generate` cannot be run from this repository (it stays as the
record of exactly how the header was made). The firmware builds from the committed header.
