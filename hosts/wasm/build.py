#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Build the Squatch Tuner browser demo: a static folder any HTTPS host can serve, at any path.

    build.py OUT_DIR [--emcc PATH]

  1. tuner.wasm: hosts/wasm/tuner_wasm.cpp and the tuner core, compiled by Emscripten as a
     standalone module with no imports (so no JavaScript glue is needed).
  2. index.html: hosts/web/index.html with each `slot:NAME` block replaced by the matching block
     of hosts/wasm/slots.html. The page itself has one source, the Pi bench's.
  3. The page's files from hosts/web and hosts/wasm, copied unchanged.

Emscripten comes from the environment (`source <emsdk>/emsdk_env.sh`) or --emcc. Standard
library only. Paths given to emcc are relative to the repository root. Every URL in the output
is relative, so it works under a sub-path such as https://<user>.github.io/squatch-tuner/.
"""
import argparse
import re
import shutil
import subprocess
import sys
from pathlib import Path

HOSTS = Path(__file__).resolve().parent.parent
ROOT = HOSTS.parent
SLOT = re.compile(r"<!-- slot:(\w+) -->\n?(.*?)\n?<!-- /slot:\1 -->", re.S)
WEB_FILES = ["tuner.css", "tokens.css", "strobe.js", "hold.js", "tuner-view.js", "favicon.svg"]
WASM_FILES = ["demo.js", "demo.css", "worklet.js"]
# The core's own rules (C++17, no exceptions, no RTTI), fixed memory, and no debug info, so the
# module carries no build paths and the same sources give the same bytes.
EMCC_FLAGS = [
    "-std=c++17", "-O3", "-DNDEBUG", "-fno-exceptions", "-fno-rtti",
    "-Wall", "-Wextra", "-Wpedantic", "-Wshadow", "-Werror",
    "--no-entry", "-sSTANDALONE_WASM=1", "-sFILESYSTEM=0",
    "-sALLOW_MEMORY_GROWTH=0", "-sINITIAL_MEMORY=1MB", "-sSTACK_SIZE=64KB",
]


def include_dirs():
    """The tuner core's headers: squatch-dsp keeps them in blocks/tuner/include, the standalone
    squatch-tuner repository in include/."""
    dirs = ["include", "blocks/tuner/include"]
    return [f"-I{d}" for d in dirs if (ROOT / d).is_dir()]


def compile_wasm(emcc, out):
    cmd = [emcc, *EMCC_FLAGS, *include_dirs(), "-o", str(out.resolve() / "tuner.wasm"), "hosts/wasm/tuner_wasm.cpp"]
    subprocess.run(cmd, check=True, cwd=ROOT)


def slots(text, where):
    found = {m.group(1): m.group(2) for m in SLOT.finditer(text)}
    if not found:
        sys.exit(f"build.py: no slots in {where}")
    return found


def page():
    base = (HOSTS / "web" / "index.html").read_text()
    mine = slots((HOSTS / "wasm" / "slots.html").read_text(), "slots.html")
    theirs = slots(base, "index.html")
    if set(mine) != set(theirs):
        sys.exit(f"build.py: slots differ: index.html has {sorted(theirs)}, slots.html has {sorted(mine)}")
    html = SLOT.sub(lambda m: mine[m.group(1)], base)
    # The Pi page's note about slots means nothing in the built demo.
    return re.sub(r"<!-- The Pi bench page.*?-->\n", "", html, count=1, flags=re.S)


def main(argv):
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("out")
    p.add_argument("--emcc", default=shutil.which("emcc"))
    args = p.parse_args(argv)
    if not args.emcc:
        sys.exit("build.py: emcc not found; run `source <emsdk>/emsdk_env.sh` first or pass --emcc")
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    compile_wasm(args.emcc, out)
    (out / "index.html").write_text(page())
    for name in WEB_FILES:
        shutil.copyfile(HOSTS / "web" / name, out / name)
    for name in WASM_FILES:
        shutil.copyfile(HOSTS / "wasm" / name, out / name)
    print(f"build.py: demo in {out}/ ({len(WEB_FILES) + len(WASM_FILES) + 2} files)")


if __name__ == "__main__":
    main(sys.argv[1:])
