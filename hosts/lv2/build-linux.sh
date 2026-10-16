#!/bin/sh
# SPDX-License-Identifier: MIT
# Builds the LV2 bundles for arm64 Linux in a Debian 12 container (glibc 2.36, so they load on
# Bookworm and anything newer, the Pi's Trixie included), runs the smoke host over them there, and
# checks what the binaries need from the target. Also copies Debian's LV2 headers to build/lv2-headers
# for the macOS build (make -C hosts/lv2 test). Needs Docker (OrbStack runs arm64 natively on this Mac).
#
#   hosts/lv2/build-linux.sh            -> build/lv2-linux-arm64/*.lv2
#   hosts/lv2/build-linux.sh --lint     ... and lv2lint (built from source in a throwaway image)
set -eu
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
image=squatch-lv2-build:bookworm
docker image inspect "$image" >/dev/null 2>&1 || docker build -t "$image" -f "$here/Containerfile" "$here"

docker run --rm --platform linux/arm64 -u "$(id -u):$(id -g)" -v "$repo:/src" -w /src "$image" sh -c '
  set -eu
  make -C hosts/lv2 -j4 test
  rm -rf build/lv2-headers && mkdir -p build/lv2-headers && cp -rL /usr/include/lv2 build/lv2-headers/
  sh hosts/lv2/inspect.sh build/lv2-linux-arm64
'
if [ "${1:-}" = "--lint" ]; then
  sh "$here/lint.sh" "$repo/build/lv2-linux-arm64"
fi
