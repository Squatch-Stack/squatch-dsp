#!/usr/bin/env bash
# Reproduce docs/PARITY-2026-10.md: browser-rig effects vs squatch-dsp blocks, same input, matched settings.
#
#   tools/parity/run.sh [OUT_DIR]        (default: build/parity)
#
# Needs: clang++, node >= 20, python3 with numpy and scipy (nothing is installed or fetched), and
#   PARITY_RIG  a squatch-rig checkout (read only)
#   NAM_CORE    the NeuralAmpModelerCore sources (the tag squatch-rig's cache holds)
#   NAM_DEPS    a directory holding eigen/ and nlohmann/ for NAM Core
# Nothing here plays audio: every render is offline, file to file.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
out="${1:-$root/build/parity}"
rig="${PARITY_RIG:?set PARITY_RIG to a squatch-rig checkout}"
nam="${NAM_CORE:?set NAM_CORE to the NAM Core sources}"
deps="${NAM_DEPS:?set NAM_DEPS to the directory holding eigen/ and nlohmann/}"
mkdir -p "$out"
cd "$root"

# 1. the library and the C++ host (NAM objects are built by `make render`)
make -j8 render NAM_CORE="$nam" NAM_EIGEN="$deps/eigen" NAM_JSON="$deps/nlohmann" >/dev/null
objs=()
while IFS= read -r o; do objs+=("$o"); done < <(find build/nam -name '*.o')
clang++ -std=c++17 -O3 -Wall -Wextra -Wpedantic -Wshadow -Werror -Iinclude -Iblocks/tuner/include \
  -DSQUATCH_WITH_NAM -DNDEBUG -DNAM_SAMPLE_FLOAT -DNAM_ENABLE_A2_FAST -DEIGEN_DONT_PARALLELIZE \
  -isystem "$nam" -isystem "$deps/eigen" -isystem "$deps/nlohmann" \
  -o build/parity-block tools/parity/parity_block.cpp "${objs[@]}" build/libsquatch-dsp.a -lpthread

# 2. inputs and cases, 3. the browser kernels, 4. the C++ blocks, 5. the numbers
python3 -I "$here/cases.py" "$rig" "$out"
node "$here/js_render.mjs" "$rig" "$out/cases.json" "$out" >"$out/js.log"
node "$here/check_harness.mjs" "$rig" "$out"
if PARITY_PERTURB=1 node "$here/check_harness.mjs" "$rig" "$out" >/dev/null 2>&1; then
  echo "negative control failed: the harness check passed on perturbed data" >&2; exit 1
fi
python3 -I "$here/selftest.py" >"$out/selftest.log" || { cat "$out/selftest.log"; exit 1; }
python3 -I "$here/run_cpp.py" "$root/build/parity-block" "$rig" "$out"
python3 -I "$here/analyze.py" "$out"
python3 -I "$here/fit_od.py" "$root/build/parity-block" "$out"
python3 -I "$here/ranges.py" "$rig" "$root" "$out" >/dev/null
echo "results: $out/results.md  ranges: $out/ranges.md  json: $out/results.json"
