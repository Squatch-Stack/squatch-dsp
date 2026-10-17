#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Flash a Seed3 image from the Mac with no button presses.

    hosts/daisy/tools/flash.py build/daisy/bin/seed3-latency-b32.bin
    hosts/daisy/tools/flash.py IMAGE.bin --backup DIR    # read the old flash into DIR first

What it does, with a timing for each step:
  1. If the STM32 ROM bootloader (USB 0483:df11) is not already there, find the Seed3's serial port by its USB serial
     number (never by /dev name), send "dfu\\n" (every image's serial loop answers it with System::ResetToBootloader),
     and wait for 0483:df11.
  2. Optionally read the whole 128 KB flash back (dfu-util -U) and write its sha256.
  3. dfu-util -a 0 -s 0x08000000:leave -D IMAGE. The trailing "get_status" error is normal on the ROM bootloader.
  4. Wait for the serial port to come back and print the image's banner (so the name proves what is running).
If no image with the dfu command is running and nothing is in DFU, it says so and exits 2 (you then press
BOOT/RESET once). Standard library only. The Arduino Nano Matter and the ESP32 boards are never touched: dfu-util
is pointed at 0483:df11 only, and the serial port is chosen by the Seed3's USB serial number.
"""
import argparse
import glob
import hashlib
import os
import re
import subprocess
import sys
import termios
import time
from pathlib import Path

SERIAL_ENV = "SEED3_SERIAL"  # the board's USB serial number; the port name /dev/cu.usbmodem<serial>... adds a suffix


def seed3_serial(value: str | None = None) -> str:
    """The Seed3's USB serial number: the argument, else $SEED3_SERIAL. Never guessed from the port list, because an
    ESP32 or any other USB board enumerates as /dev/cu.usbmodem* too and must never be written to."""
    serial = value or os.environ.get(SERIAL_ENV, "")
    if not re.fullmatch(r"[0-9A-Za-z]{4,32}", serial):
        sys.exit(f"flash: set {SERIAL_ENV} (or --serial) to your Seed3's USB serial number: the part of its port name "
                 "after /dev/cu.usbmodem with the trailing digit removed (ls /dev/cu.usbmodem* with only the Seed3 plugged in)")
    return serial
DFU_ID = "0483:df11"
FLASH_BASE = 0x08000000
FLASH_SIZE = 128 * 1024


def log(t0: float, msg: str) -> None:
    print(f"[{time.monotonic() - t0:6.2f}s] {msg}", flush=True)


def dfu_devices() -> list[str]:
    out = subprocess.run(["dfu-util", "-l"], capture_output=True, text=True).stdout
    return [ln for ln in out.splitlines() if f"[{DFU_ID}]" in ln and "alt=0" in ln]


def find_port(serial: str) -> str | None:
    ports = sorted(glob.glob(f"/dev/cu.usbmodem{serial}*"))
    return ports[0] if ports else None


def wait_for(pred, timeout: float, step: float = 0.1):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        v = pred()
        if v:
            return v
        time.sleep(step)
    return None


def open_raw(path: str) -> int:
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    attrs = termios.tcgetattr(fd)
    attrs[0] = attrs[1] = attrs[3] = 0  # iflag, oflag, lflag: raw
    attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    return fd


def read_lines(path: str, seconds: float) -> list[str]:
    fd = open_raw(path)
    buf = b""
    end = time.monotonic() + seconds
    try:
        while time.monotonic() < end:
            try:
                chunk = os.read(fd, 4096)
                if chunk:
                    buf += chunk
            except BlockingIOError:
                time.sleep(0.02)
    finally:
        os.close(fd)
    return buf.decode("utf-8", "replace").replace("\r", "").split("\n")


def send_dfu(path: str) -> None:
    fd = open_raw(path)
    try:
        os.write(fd, b"dfu\n")
        time.sleep(0.2)
    finally:
        os.close(fd)


def parse_args() -> argparse.Namespace:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("image", type=Path)
    ap.add_argument("--serial", help=f"the Seed3's USB serial number (default: ${SERIAL_ENV})")
    ap.add_argument("--backup", type=Path, help="directory: read the old flash into it before writing")
    ap.add_argument("--no-wait-port", action="store_true")
    return ap.parse_args()


def enter_dfu(t0: float, serial: str) -> int:
    """0 once the ROM bootloader is up (sending 'dfu' if needed), else the exit status to return."""
    devs = dfu_devices()
    if len(devs) > 1:
        print(f"flash: {len(devs)} DFU devices at {DFU_ID}; refusing to guess", file=sys.stderr)
        return 1
    if devs:
        log(t0, "already in the ROM bootloader")
        return 0
    port = find_port(serial)
    if not port:
        print(f"flash: no Seed3 serial port (USB serial {serial}) and no {DFU_ID} device: "
              "press BOOT/RESET on the Daisy once", file=sys.stderr)
        return 2
    log(t0, f"sending 'dfu' to {port}")
    send_dfu(port)
    if not wait_for(lambda: len(dfu_devices()) == 1, 15):
        print("flash: no DFU device after the 'dfu' command (this image may predate it, or the board did not reset):"
              " press BOOT/RESET once", file=sys.stderr)
        return 2
    log(t0, f"{DFU_ID} is up")
    return 0


def backup_flash(t0: float, directory: Path) -> int:
    directory.mkdir(parents=True, exist_ok=True)
    old = directory / time.strftime("daisy-flash-%Y%m%d-%H%M%S.bin")
    subprocess.run(["dfu-util", "-d", f",{DFU_ID}", "-a", "0", "-s", f"0x{FLASH_BASE:08x}:{FLASH_SIZE}", "-U", str(old)],
                   capture_output=True, text=True)
    if not (old.is_file() and old.stat().st_size == FLASH_SIZE):
        print(f"flash: backup read failed ({old})", file=sys.stderr)
        return 1
    sha = hashlib.sha256(old.read_bytes()).hexdigest()
    old.with_suffix(".sha256").write_text(f"{sha}  {old.name}\n")
    log(t0, f"backed up the old flash: {old.name} sha256 {sha}")
    return 0


def write_image(t0: float, image: Path) -> int:
    log(t0, f"flashing {image.name} ({image.stat().st_size} bytes)")
    r = subprocess.run(["dfu-util", "-d", f",{DFU_ID}", "-a", "0", "-s", f"0x{FLASH_BASE:08x}:leave", "-D", str(image)],
                       capture_output=True, text=True)
    text = r.stdout + r.stderr
    if "Download done" not in text and "File downloaded successfully" not in text:
        print(text, file=sys.stderr)
        print("flash: dfu-util did not report a finished download", file=sys.stderr)
        return 1
    log(t0, "download done (a trailing get_status error is normal)")
    return 0


def wait_for_image(t0: float, serial: str) -> int:
    port = wait_for(lambda: find_port(serial), 20)
    if not port:
        print("flash: the serial port did not come back in 20 s", file=sys.stderr)
        return 1
    log(t0, f"serial port is back: {port}")
    banners = [ln for ln in read_lines(port, 4.0) if ln.startswith("# ")]
    for ln in banners[:2]:
        print("  " + ln)
    log(t0, "running" if banners else "no banner within 4 s (banners come every 10 s; the port is up)")
    return 0


def main() -> int:
    a = parse_args()
    a.serial = seed3_serial(a.serial)
    if not a.image.is_file():
        print(f"flash: no such image {a.image}", file=sys.stderr)
        return 1
    if a.image.stat().st_size > FLASH_SIZE:
        print(f"flash: {a.image} is over the {FLASH_SIZE}-byte internal flash", file=sys.stderr)
        return 1
    t0 = time.monotonic()
    steps = [lambda: enter_dfu(t0, a.serial)]
    if a.backup:
        steps.append(lambda: backup_flash(t0, a.backup))
    steps.append(lambda: write_image(t0, a.image))
    if not a.no_wait_port:
        steps.append(lambda: wait_for_image(t0, a.serial))
    for step in steps:
        rc = step()
        if rc:
            return rc
    return 0


if __name__ == "__main__":
    sys.exit(main())
