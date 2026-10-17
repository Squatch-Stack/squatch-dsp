#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Tests for the guards in hosts/daisy/tools and faust/generate. No board, no network, standard library only
(the accuracy-gate test needs numpy and is skipped without it).

    python3 hosts/daisy/tests/test_tools.py

Every guard has a negative control: a case that must be refused, next to the case that must pass.
"""
import importlib.machinery
import importlib.util
import os
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

DAISY = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(DAISY / "tools"))
import flash  # noqa: E402


def load_script(path: Path, name: str):
    loader = importlib.machinery.SourceFileLoader(name, str(path))
    spec = importlib.util.spec_from_loader(name, loader)
    module = importlib.util.module_from_spec(spec)
    loader.exec_module(module)
    return module


class SerialNumberGuard(unittest.TestCase):
    """flash.py writes to whatever it finds, so it never guesses the board from the port list."""

    def setUp(self):
        self.saved = os.environ.pop(flash.SERIAL_ENV, None)

    def tearDown(self):
        os.environ.pop(flash.SERIAL_ENV, None)
        if self.saved is not None:
            os.environ[flash.SERIAL_ENV] = self.saved

    def test_missing_serial_is_refused(self):
        with self.assertRaises(SystemExit) as cm:
            flash.seed3_serial()
        self.assertIn("SEED3_SERIAL", str(cm.exception))

    def test_garbage_serial_is_refused(self):
        for bad in ("", "../etc", "ab", "a b", "x" * 40, "12*", "$(id)"):
            with self.subTest(bad=bad), self.assertRaises(SystemExit):
                flash.seed3_serial(bad)

    def test_valid_serial_is_accepted(self):
        self.assertEqual(flash.seed3_serial("ABC123def456"), "ABC123def456")  # the control: a good one passes
        os.environ[flash.SERIAL_ENV] = "0123456789AB"
        self.assertEqual(flash.seed3_serial(), "0123456789AB")
        self.assertEqual(flash.seed3_serial("FEDCBA987654"), "FEDCBA987654")  # the argument wins over the environment


class FetchNamModel(unittest.TestCase):
    """fetch-nam-model must leave no model behind when the download is not the pinned file."""

    SCRIPT = DAISY / "tools" / "fetch-nam-model"

    def run_with_fake_curl(self, payload: bytes, script_edit=None):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            tools = root / "hosts/daisy/tools"
            tools.mkdir(parents=True)
            text = self.SCRIPT.read_text()
            if script_edit:
                text = script_edit(text)
            (tools / "fetch-nam-model").write_text(text)
            (tools / "fetch-nam-model").chmod(0o755)
            bin_dir = root / "bin"
            bin_dir.mkdir()
            (root / "payload").write_bytes(payload)
            curl = bin_dir / "curl"
            # the fake curl copies the payload to the file named after -o and checks it was asked for https
            curl.write_text('#!/bin/sh\nwhile [ $# -gt 0 ]; do case "$1" in -o) out="$2"; shift;; '
                            'https://*) url="$1";; esac; shift; done\n[ -n "${url:-}" ] || exit 9\n'
                            f'cp "{root / "payload"}" "$out"\n')
            curl.chmod(curl.stat().st_mode | stat.S_IXUSR)
            env = dict(os.environ, PATH=f"{bin_dir}{os.pathsep}{os.environ['PATH']}")
            r = subprocess.run(["sh", str(tools / "fetch-nam-model")], env=env, capture_output=True, text=True)
            model = root / "build/nam-model/A2.nam"
            leftovers = sorted(p.name for p in (root / "build/nam-model").glob("*")) if model.parent.exists() else []
            return r, leftovers

    def test_wrong_file_is_refused_and_removed(self):
        r, leftovers = self.run_with_fake_curl(b"not the model")
        self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
        self.assertIn("sha256", r.stderr)
        self.assertEqual(leftovers, [], "a refused download must leave nothing behind")

    def test_pinned_file_is_accepted(self):
        import hashlib
        payload = b"the pinned model, for this test"
        digest = hashlib.sha256(payload).hexdigest()
        r, leftovers = self.run_with_fake_curl(
            payload, lambda text: "\n".join(f"SHA256={digest}" if ln.startswith("SHA256=") else ln for ln in text.splitlines()))
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertEqual(leftovers, ["A2.nam"])


class FaustAdaptGuard(unittest.TestCase):
    """faust/generate edits Faust's output in exact places; if Faust's output changes shape it must stop, not guess."""

    @classmethod
    def setUpClass(cls):
        cls.gen = load_script(DAISY / "faust" / "generate", "faust_generate")

    def test_once_refuses_zero_and_two_matches(self):
        self.assertEqual(self.gen.once("a b", "b", "c", "thing"), "a c")  # the control: exactly one match passes
        for text in ("a", "b b"):
            with self.subTest(text=text), self.assertRaises(SystemExit):
                self.gen.once(text, "b", "c", "thing")

    def test_adapt_refuses_output_that_still_allocates(self):
        committed = (DAISY / "faust" / "drive_faust.hpp").read_text()
        self.assertNotIn("virtual", committed.split("*/", 1)[1])  # the committed file passed the same bans
        with self.assertRaises(SystemExit):
            self.gen.adapt("class DriveFaust {}")  # not Faust's shape at all


class Notices(unittest.TestCase):
    """The licence notices the generated and third-party files must carry."""

    REQUIRED = {
        "faust/drive_faust.hpp": ["Julius O. Smith III", "STK", "LGPL", "THE SOFTWARE IS PROVIDED"],
        "THIRD_PARTY.md": ["Steven Atkinson", "Electrosmith", "Julius O. Smith III", "0001-cmake-define-DATA_IN_D2_SRAM",
                           "0002-linker-scripts-drop-malformed-DISCARD", "LGPL"],
        "nam/a2lite_weights.hpp": ["Steven Atkinson", "MIT"],
    }

    @staticmethod
    def missing(text: str, needles) -> list:
        return [n for n in needles if n not in text]

    def test_notices_are_present(self):
        for rel, needles in self.REQUIRED.items():
            with self.subTest(file=rel):
                self.assertEqual(self.missing((DAISY / rel).read_text(), needles), [])

    def test_the_check_notices_a_removed_notice(self):  # negative control
        text = (DAISY / "THIRD_PARTY.md").read_text().replace("Steven Atkinson", "someone")
        self.assertEqual(self.missing(text, ["Steven Atkinson"]), ["Steven Atkinson"])


class AccuracyGate(unittest.TestCase):
    def test_gate_rejects_a_wrong_variant_and_a_false_exactness_claim(self):
        try:
            import numpy  # noqa: F401
        except ImportError:
            self.skipTest("numpy is not installed")
        bench = load_script(DAISY / "tools" / "nam_bench.py", "nam_bench")
        exact = bench.Variant("v", (), "why")
        self.assertEqual(bench.judge(exact, -123.9, 4800, 4800, -100.0), [])  # passes
        self.assertTrue(bench.judge(exact, -40.0, 0, 4800, -100.0), "ESR -40 dB must be refused")
        self.assertTrue(bench.judge(exact, -123.9, 4799, 4800, -100.0), "one differing sample breaks an exact claim")
        inexact = bench.Variant("v", (), "why", exact=False)
        self.assertEqual(bench.judge(inexact, -123.0, 175, 4800, -100.0), [])
        self.assertTrue(bench.judge(inexact, -40.0, 0, 4800, -100.0), "the accuracy gate alone must refuse ESR -40 dB")
        self.assertIn("xbreak", bench.BREAK.tokens)
        self.assertTrue(bench.BREAK.expect_fail)


class FetchLibdaisy(unittest.TestCase):
    """fetch-libdaisy --verify must refuse a checkout that is not the pinned one."""

    def test_missing_checkout_is_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            copy = Path(tmp) / "hosts/daisy"
            copy.mkdir(parents=True)
            shutil.copy(DAISY / "fetch-libdaisy", copy / "fetch-libdaisy")
            shutil.copytree(DAISY / "patches", copy / "patches")
            r = subprocess.run(["sh", str(copy / "fetch-libdaisy"), "--verify"], capture_output=True, text=True)
            self.assertNotEqual(r.returncode, 0)
            self.assertIn("no checkout", r.stderr)

    def test_stamp_from_other_patches_is_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            copy = Path(tmp) / "hosts/daisy"
            (copy / "vendor/libDaisy").mkdir(parents=True)
            shutil.copy(DAISY / "fetch-libdaisy", copy / "fetch-libdaisy")
            shutil.copytree(DAISY / "patches", copy / "patches")
            (copy / "vendor/libDaisy/.squatch-pin").write_text("libDaisy something-else\n")
            r = subprocess.run(["sh", str(copy / "fetch-libdaisy"), "--verify"], capture_output=True, text=True)
            self.assertNotEqual(r.returncode, 0)
            self.assertIn("another pin", r.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=1)
