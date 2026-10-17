#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""A2-Lite kernel optimisation pipeline for the Seed3: build, flash, time on the board, check accuracy, write a table.

    hosts/daisy/tools/nam_bench.py                       # every variant in VARIANTS, results under build/daisy/nam-bench/
    hosts/daisy/tools/nam_bench.py --only fused,dtcm     # a subset (names or token strings)
    hosts/daisy/tools/nam_bench.py --selftest            # also run the deliberately wrong variant: it MUST be rejected
    hosts/daisy/tools/nam_bench.py --no-flash            # build and size only

For each variant (a list of kernel options, see the NAMOPT_* list at the top of hosts/daisy/nam/a2lite.hpp):
  1. builds the image `seed3-nam-bench-b32-<tokens>` (make EXTRA_VARIANTS=...), and reads flash and RAM from the link;
  2. flashes it with tools/flash.py, no button press (the bench image runs the model from the main loop with no audio,
     so even a kernel over budget cannot starve the serial port);
  3. reads "# bench:" (mean, min and max cycles per block over 150 steady-state blocks) and the 4,800-sample recording;
  4. compares the recording with NAM's float64 reference (tools/a2_reference.py) fed the board's own input bits, and with
     the same kernel built for this Mac (tools/nam_host.cpp, same options);
  5. writes results.csv and results.md.
It fails loudly: a variant is FAIL (and the exit status is 1) when the image does not run, its banner is not the one
asked for, the stimulus differs from the baseline's, the recording is incomplete, the ESR against the reference is worse
than --max-esr (default -100 dB), or an "exact" variant (one that must not change a single output bit) differs from the
baseline kernel's output on the board in even one sample. --selftest adds `xbreak` (one weight scaled by 1.01), which the gate must
reject; if it does not, the pipeline reports that the gate itself is broken.
Hangs: if an image gives no serial in time, it asks you (on the terminal) to press BOOT/RESET and waits up to
30 minutes for 0483:df11 (a fault lands there by itself, see flash.py). Standard library plus numpy.
"""
import argparse
import csv
import hashlib
import math
import re
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import flash  # noqa: E402
import nam_check  # noqa: E402

import numpy as np  # noqa: E402

ROOT = HERE.parents[2]
DAISY = ROOT / "hosts/daisy"
BUDGET = 320000  # cycles in a 32-sample block at 480 MHz / 48 kHz
REC_BUFFERS = 2 * 4800 * 4 - 8  # the bench image's recording arrays, left out of the RAM column
FLAG_TOKENS = {"xo2", "xo3", "xos", "xfast", "xlto"}  # change the compiler or linker, not a source option


@dataclass
class Variant:
    name: str
    tokens: tuple
    why: str
    exact: bool = True  # must be bit-identical to the baseline kernel on the Mac
    expect_fail: bool = False
    timing_only: bool = False  # a diagnostic ablation: output is wrong on purpose, only the cycles count


# The default sweep: every step on its own and the combinations, in the order they were tried.
VARIANTS = [
    Variant("base", (), "first float32 kernel, rings and state in AXI SRAM, weights in flash"),
]
BREAK = Variant("break", ("xbreak",), "NEGATIVE CONTROL: one weight x1.01; the gate must reject it", exact=False, expect_fail=True)


def load_variants(path: Path | None) -> list[Variant]:
    """Extra variants from a file of lines 'name: tokens...  # why  [inexact]' (the sweep is edited there, not here)."""
    out = list(VARIANTS)
    if path and path.is_file():
        for ln in path.read_text().splitlines():
            ln = ln.strip()
            if not ln or ln.startswith("//"):
                continue
            body, _, why = ln.partition("#")
            name, _, toks = body.partition(":")
            out.append(Variant(name.strip(), tuple(toks.split()), why.replace("[inexact]", "").replace("[timing-only]", "").strip(),
                               exact="[inexact]" not in why and "[timing-only]" not in why, timing_only="[timing-only]" in why))
    return out


def git_info() -> str:
    def g(*a):
        return subprocess.run(["git", "-C", str(ROOT), *a], capture_output=True, text=True).stdout.strip()
    dirty = "+dirty" if g("status", "--porcelain", "hosts/daisy", "include", "src") else ""
    return f"{g('rev-parse', '--short', 'HEAD')}{dirty} on {g('rev-parse', '--abbrev-ref', 'HEAD')}"


def image_name(v: Variant, block: int) -> str:
    return "nam-bench-b%d%s" % (block, "".join("-" + t for t in v.tokens))


def macro_flags(tokens) -> list[str]:
    return [f"-DNAMOPT_{t[1:].upper()}=1" for t in tokens if t.startswith("x") and t not in FLAG_TOKENS]


def build(v: Variant, block: int, log) -> dict:
    name = image_name(v, block)
    t0 = time.monotonic()
    r = subprocess.run(["make", "-C", str(DAISY), "-j4", f"EXTRA_VARIANTS={name}", f"../../build/daisy/bin/seed3-{name}.bin"],
                       capture_output=True, text=True)
    secs = time.monotonic() - t0
    if r.returncode:
        log.write(r.stdout[-3000:] + r.stderr[-6000:])
        raise RuntimeError(f"build of {name} failed (see build.log)")
    mem = (ROOT / f"build/daisy/{name}/memory.txt").read_text()
    used = {}
    for m in re.finditer(r"^\s*(\w+):\s+(\d+) (B|KB|MB)\s", mem, re.M):
        used[m.group(1)] = int(m.group(2)) * {"B": 1, "KB": 1024, "MB": 1 << 20}[m.group(3)]
    image = ROOT / f"build/daisy/bin/seed3-{name}.bin"
    return dict(name=name, image=image, secs=secs, flash=used.get("FLASH", image.stat().st_size),
                ram=used.get("SRAM", 0) + used.get("DTCMRAM", 0) + used.get("ITCMRAM", 0) - REC_BUFFERS,
                dtcm=used.get("DTCMRAM", 0), itcm=used.get("ITCMRAM", 0), bytes=image.stat().st_size)


def ask_for_press(why: str) -> None:
    print(f"nam_bench: press BOOT/RESET on the Daisy now (hold BOOT, tap RESET, release BOOT). {why} "
          "Waiting up to 30 min for 0483:df11, then continuing by itself.", flush=True)
    if not flash.wait_for(lambda: flash.dfu_devices(), 1800, step=2.0):
        raise RuntimeError("no BOOT/RESET press within 30 minutes")


def flash_image(image: Path, log) -> float:
    t0 = time.monotonic()
    for attempt in (1, 2):
        r = subprocess.run([sys.executable, str(HERE / "flash.py"), str(image)], capture_output=True, text=True)
        log.write(r.stdout + r.stderr)
        if r.returncode == 0:
            return time.monotonic() - t0
        if r.returncode == 2 and attempt == 1:
            ask_for_press("The previous image has no `dfu` command (or hung).")
            continue
        raise RuntimeError(f"flash.py failed (exit {r.returncode}); see build.log")
    raise RuntimeError("unreachable")


def collect_lines(name: str, seconds: float) -> list[str]:
    """Serial lines until a bench line and a whole DUMP block (600 rows of 8 samples) have arrived, or the time is up."""
    t_end = time.monotonic() + seconds
    lines: list[str] = []
    while time.monotonic() < t_end:
        port = flash.find_port(flash.seed3_serial())
        if not port:
            time.sleep(0.5)
            continue
        try:
            lines += flash.read_lines(port, 6.0)
        except OSError:
            time.sleep(0.5)
        rows = {int(m.group(1)) for m in (re.match(r"D (\d+) ", ln) for ln in lines) if m}
        if len(rows) >= 600 and "# bench:" in "\n".join(lines):
            break
    return lines


def check_banner(name: str, lines: list[str]) -> None:
    banner = next((ln for ln in lines if ln.startswith("# seed3-")), "")
    if f"seed3-{name} " not in banner + " ":
        raise RuntimeError(f"banner is {banner!r}, expected seed3-{name}: the wrong image is running")


def parse_bench(name: str, lines: list[str]) -> dict:
    """The image's own report: banner (checked), '# bench:' numbers and '# options:' list."""
    check_banner(name, lines)
    pat = r"# bench: block=(\d+) mean=(\d+) min=(\d+) max=(\d+) warm=(\d+) blocks=(\d+) budget=(\d+)"
    found = [m for m in (re.match(pat, ln) for ln in lines) if m]
    if not found:
        raise RuntimeError("no '# bench:' line from the board")
    keys = ("block", "mean", "min", "max", "warm", "blocks", "budget")
    bench = {k: int(v) for k, v in zip(keys, found[-1].groups())}
    bench["options"] = image_options(lines)
    return bench


def image_options(lines: list[str]) -> list[str]:
    opt_lines = [ln for ln in lines if ln.startswith("# options:")]
    if not opt_lines:
        raise RuntimeError("the image printed no '# options:' line")
    return sorted(opt_lines[-1].split(":", 1)[1].split())


def read_board(name: str, log, seconds: float = 45.0) -> tuple[dict, list[str]]:
    lines = collect_lines(name, seconds)
    log.write("\n".join(lines[-80:]) + "\n")
    return parse_bench(name, lines), lines


def host_run(tag: str, flags: list[str], x: np.ndarray, work: Path) -> np.ndarray:
    exe = work / f"nam_host_{tag}"
    subprocess.run(["clang++", "-std=c++17", "-O2", *flags, f"-I{ROOT / 'include'}", str(HERE / "nam_host.cpp"), "-o", str(exe)], check=True)
    (work / "in.f32").write_bytes(x.astype("<f4").tobytes())
    subprocess.run([str(exe), str(work / "in.f32"), str(work / f"out_{tag}.f32")], check=True)
    return np.fromfile(work / f"out_{tag}.f32", dtype="<f4")


def float64_reference(x: np.ndarray, work: Path, model: Path) -> np.ndarray:
    x.astype("<f8").tofile(work / "in.f64")
    subprocess.run([sys.executable, str(nam_check.REF), str(model), "--slim", "0.0", "--input", str(work / "in.f64"),
                    "--out", str(work / "ref.f64")], check=True, capture_output=True)
    return np.fromfile(work / "ref.f64", dtype="<f8")


def human(n: int) -> str:
    return f"{n / 1024:.1f} KB"


def same_bits(a: np.ndarray, b: np.ndarray) -> int:
    return int(np.sum(a.view(np.uint32) == b.view(np.uint32)))


@dataclass
class Session:
    """What every variant is compared with: the baseline's stimulus and outputs, and NAM's float64 reference."""
    args: argparse.Namespace
    outdir: Path
    work: Path
    log: object
    ref: np.ndarray | None = None
    base_in: np.ndarray | None = None
    base_host: np.ndarray | None = None
    base_y: np.ndarray | None = None


def empty_row(v: Variant) -> dict:
    cols = ("cycles_mean", "cycles_min", "cycles_max", "pct_budget", "esr_db", "exact_vs_base", "flash_bytes", "ram_bytes",
            "dtcm_bytes", "stack_used", "stack_region", "flash_secs", "note")
    return dict(variant=v.name, options=" ".join(v.tokens) or "(none)", why=v.why, status="", **{c: "" for c in cols})


def adopt_baseline(ses: Session, x: np.ndarray, y: np.ndarray) -> None:
    """The first variant run (the baseline) fixes the stimulus; later ones must have been fed the very same bits."""
    if ses.base_in is None:
        ses.base_in, ses.base_y = x.copy(), y.copy()
        ses.ref = float64_reference(x, ses.work, Path(ses.args.model))
        ses.base_host = host_run("base", [], x, ses.work)
    elif not np.array_equal(ses.base_in.view(np.uint32), x.view(np.uint32)):
        raise RuntimeError("the board's input differs from the baseline's: the stimulus is not the same")


def measure(v: Variant, b: dict, ses: Session, row: dict) -> None:
    """Flash, run, read and judge one image; raises AssertionError when the accuracy gate or the exactness claim fails."""
    row["flash_secs"] = f"{flash_image(b['image'], ses.log):.1f}"
    bench, lines = read_board(b["name"], ses.log)
    asked = sorted(("FASTMATH" if t == "xfast" else t[1:].upper()) for t in v.tokens if t not in FLAG_TOKENS - {"xfast"})
    if bench["options"] != asked:
        raise RuntimeError(f"the image was built with options {bench['options']}, not the {asked} that were asked for")
    row.update(stack_fields(lines))
    x, y = nam_check.read_dump(lines)
    if len(x) != 4800:
        raise RuntimeError(f"recording has {len(x)} samples, expected 4800")
    adopt_baseline(ses, x, y)
    host_v = host_run(re.sub(r"\W", "_", v.name), macro_flags(v.tokens), x, ses.work) if v.tokens else ses.base_host
    if not v.timing_only and not np.all(np.isfinite(y)):
        raise RuntimeError("the board's output has NaN or infinity")
    esr, same_base = nam_check.esr_db(y, ses.ref), same_bits(y, ses.base_y)
    row.update(cycles_mean=bench["mean"], cycles_min=bench["min"], cycles_max=bench["max"], esr_db=f"{esr:.1f}",
               pct_budget=f"{100.0 * bench['mean'] / BUDGET:.1f}", exact_vs_base=f"{same_base}/{len(y)}")
    row["note"] = f"board vs Mac same options {same_bits(y, host_v)}/{len(y)}, vs Mac baseline {same_bits(y, ses.base_host)}/{len(y)}"
    problems = judge(v, esr, same_base, len(y), ses.args.max_esr)
    if problems:
        raise AssertionError("; ".join(problems))
    print(f"[{v.name}] {bench['mean']:,} cycles/block ({row['pct_budget']}%), min {bench['min']:,} max {bench['max']:,}; "
          f"ESR {esr:.1f} dB; exact vs baseline {same_base}/{len(y)}", flush=True)


def stack_fields(lines: list[str]) -> dict:
    """The stack high-water mark the bench image painted and read (seed3-nam/main.cpp), if it printed one."""
    found = [m for m in (re.match(r"# stack: region=(\d+) used_hw=(\d+)", ln) for ln in lines) if m]
    return dict(stack_region=found[-1].group(1), stack_used=found[-1].group(2)) if found else {}


def judge(v: Variant, esr: float, same_base: int, n: int, max_esr: float) -> list[str]:
    problems = []
    if not v.timing_only and esr > max_esr:
        problems.append(f"ESR {esr:.1f} dB is worse than {max_esr:.0f} dB")
    if v.exact and not v.timing_only and same_base != n:
        problems.append(f"declared exact but only {same_base}/{n} samples equal the baseline kernel's output on the board")
    return problems


def build_and_measure(v: Variant, ses: Session, row: dict) -> None:
    b = build(v, ses.args.block, ses.log)
    row.update(flash_bytes=b["flash"], ram_bytes=b["ram"], dtcm_bytes=b["dtcm"])
    print(f"[{v.name}] built {b['name']}: flash {human(b['flash'])}, ram {human(b['ram'])} (dtcm {human(b['dtcm'])}) "
          f"in {b['secs']:.1f}s", flush=True)
    if ses.args.no_flash:
        row["status"] = "built"
        return
    for _ in range(ses.args.repeat):
        measure(v, b, ses, row)
    row["status"] = "timing-only" if v.timing_only else "ok"
    if v.timing_only:
        row["note"] = "diagnostic, output wrong on purpose"


def run_variant(v: Variant, ses: Session, failures: list[str]) -> dict:
    """Build, size and (unless --no-flash) measure one variant; every failure becomes a row, never a silent skip."""
    row = empty_row(v)
    try:
        build_and_measure(v, ses, row)
        if v.expect_fail:
            row["status"], row["note"] = "FAIL", "the wrong variant PASSED: the accuracy gate is broken"
    except AssertionError as e:
        row["status"], row["note"] = ("REJECTED" if v.expect_fail else "FAIL"), (row["note"] + " " + str(e)).strip()
    except Exception as e:  # build, flash, serial or parse problems
        row["status"], row["note"] = "FAIL", str(e)
    if row["status"] not in ("ok", "built", "timing-only"):
        print(f"[{v.name}] {row['status']}: {row['note']}", file=sys.stderr if row["status"] == "FAIL" else sys.stdout, flush=True)
    if row["status"] == "FAIL":
        failures.append(v.name)
    return row


def parse_args() -> argparse.Namespace:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--out", type=Path, default=ROOT / "build/daisy/nam-bench")
    ap.add_argument("--variants", type=Path, default=HERE / "nam_variants.txt", help="file of extra variants")
    ap.add_argument("--only", help="comma-separated variant names or token strings")
    ap.add_argument("--block", type=int, default=32)
    ap.add_argument("--max-esr", type=float, default=-100.0, help="ESR gate in dB against NAM's float64 reference")
    ap.add_argument("--model", default=str(nam_check.DEFAULT_MODEL), help="NAM Core's A2.nam (tools/fetch-nam-model)")
    ap.add_argument("--selftest", action="store_true", help="also run the wrong variant: it must be rejected")
    ap.add_argument("--no-flash", action="store_true", help="build and size only")
    ap.add_argument("--repeat", type=int, default=1, help="flash and measure each variant this many times")
    return ap.parse_args()


def select_variants(a: argparse.Namespace) -> list[Variant]:
    variants = load_variants(a.variants)
    if a.only:
        want = {w.strip() for w in a.only.split(",")}
        variants = [v for v in variants if v.name in want or "-".join(v.tokens) in want]
    if not any(v.name == "base" for v in variants):  # every comparison is against the baseline kernel, measured in this run
        variants.insert(0, VARIANTS[0])
    if a.selftest:
        variants.append(BREAK)
    return variants


def main() -> int:
    a = parse_args()
    variants = select_variants(a)
    outdir = a.out / time.strftime("%Y%m%d-%H%M%S")
    outdir.mkdir(parents=True, exist_ok=True)
    (outdir / "work").mkdir()
    ses = Session(args=a, outdir=outdir, work=outdir / "work", log=(outdir / "build.log").open("w"))
    print(f"nam_bench: {len(variants)} variants, source {git_info()}, results in {outdir}", flush=True)
    rows, failures = [], []
    for v in variants:
        rows.append(run_variant(v, ses, failures))
        write_tables(outdir, rows, a, git_info())
    print(f"nam_bench: {outdir / 'results.md'}")
    if failures:
        print(f"nam_bench: FAILED variants: {', '.join(failures)}", file=sys.stderr)
        return 1
    return 0


def md_row(r: dict, base: dict | None) -> str:
    has = r["cycles_mean"] != ""
    speed = f"{int(base['cycles_mean']) / int(r['cycles_mean']):.2f}x" if base and has else ""
    cycles = f"{int(r['cycles_mean']):,}" if has else ""
    minmax = f"{int(r['cycles_min']):,} / {int(r['cycles_max']):,}" if r["cycles_min"] != "" else ""
    flash = human(int(r["flash_bytes"])) if r["flash_bytes"] != "" else ""
    ram = f"{human(int(r['ram_bytes']))} ({human(int(r['dtcm_bytes']))})" if r["ram_bytes"] != "" else ""
    note = " " + r["note"] if r["status"] not in ("ok", "built") and r["note"] else ""
    return (f"| {r['variant']} | {r['options']} | {cycles} | {r['pct_budget']} | {speed} | {minmax} | {r['esr_db']} | "
            f"{r['exact_vs_base']} | {flash} | {ram} | {r['stack_used']} / {r['stack_region']} | {r['status']}{note} |")


def write_tables(outdir: Path, rows: list, a, src: str) -> None:
    with (outdir / "results.csv").open("w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        w.writeheader()
        w.writerows(rows)
    base = next((r for r in rows if r["variant"] == "base" and r["cycles_mean"] != ""), None)
    md = ["# A2-Lite on the Seed3, kernel variants (measured)", "",
          f"Source {src}; block {a.block}, budget {BUDGET:,} cycles/block; ESR gate {a.max_esr:.0f} dB against NAM's float64 reference; "
          f"cycles are the mean of 150 steady-state blocks on the board (min and max beside it). RAM excludes the bench's recording buffers.", "",
          "| variant | options | cycles/block | % of budget | speed-up | min / max | ESR dB | = baseline | flash | RAM (DTCM) | stack used / region | status |",
          "|---|---|---|---|---|---|---|---|---|---|---|---|"]
    md += [md_row(r, base) for r in rows]
    (outdir / "results.md").write_text("\n".join(md) + "\n")


if __name__ == "__main__":
    sys.exit(main())
