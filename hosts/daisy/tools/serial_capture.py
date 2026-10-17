#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Log the Seed3's USB serial output to a file for a number of seconds (by its USB serial number, never by /dev name).

    hosts/daisy/tools/serial_capture.py SECONDS OUT.txt [--send "dfu"]

Lines are written as they arrive; the file is the raw log. Standard library
only; shares its port handling with flash.py.
"""
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import flash  # noqa: E402


def main() -> int:
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    secs, out = float(sys.argv[1]), Path(sys.argv[2])
    port = flash.wait_for(lambda: flash.find_port(flash.seed3_serial()), 20)
    if not port:
        print("serial_capture: no Seed3 serial port", file=sys.stderr)
        return 1
    t0 = time.monotonic()
    lines = flash.read_lines(port, secs)
    out.write_text("\n".join(lines) + "\n")
    print(f"serial_capture: {len([x for x in lines if x])} lines in {time.monotonic() - t0:.1f} s -> {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
