"""Tests for triage.py: strings heuristics, verdict, and a full run on a synthetic game (image + libs, no SDK)."""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

TOOLS = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROOT = os.path.dirname(TOOLS)
sys.path.insert(0, TOOLS)
sys.path.insert(0, os.path.join(TOOLS, "tests"))
import import_coverage as IC           # noqa: E402
import triage                           # noqa: E402
from test_imports_join import make_lib  # noqa: E402
from test_step0 import build_image      # noqa: E402
from test_v2_to_v3 import make_dso      # noqa: E402


class Strings(unittest.TestCase):
    def test_real_paths_and_junk(self):
        raw = (b"\0c:\\system\\apps\\game\\data.bin\0e:\\games\\foo\0\\system\\apps\\x\0"
               b"(:,:0:4:8:<:@:D:H:L:P:T:X:\\:`:d:h:l:p:t:x:|:\0H:\\\"\0loading.bin\0\\sub\\file.dat\0")
        absolute, files, _ = triage.strings_of_interest(raw)
        found = [t for _, _, t in absolute]
        self.assertEqual(sorted(found), sorted(["c:\\system\\apps\\game\\data.bin", "e:\\games\\foo", "\\system\\apps\\x"]))
        self.assertIn("loading.bin", [t for _, _, t in files])

    def test_banner(self):
        _, _, banners = triage.strings_of_interest(b"\0Hekkus Sound System ver. 0.99 by someone\0")
        self.assertEqual(len(banners), 1)


class Verdict(unittest.TestCase):
    def test_levels(self):
        js = {"mode": "arm", "mixed": False}
        self.assertEqual(triage.verdict(js, {"trap_work": []}, 100, 1024)[0], "READY TO TRY")
        self.assertEqual(triage.verdict(js, {"trap_work": [1]}, 100, 1024)[0], "NEEDS WORK")
        self.assertEqual(triage.verdict(js, {"trap_work": []}, 2000, 1024)[0], "BLOCKED")
        self.assertEqual(triage.verdict({"mode": "thumb", "mixed": False}, {"trap_work": []}, 10, 1024)[0], "BLOCKED")
        self.assertEqual(triage.verdict({"mode": "arm", "mixed": True}, {"trap_work": [1]}, 10, 1024)[0], "BLOCKED")


class EndToEnd(unittest.TestCase):
    def setUp(self):
        self.d = tempfile.mkdtemp(prefix="tri_")
        reg, _ = IC.load_shim_registry(os.path.join(ROOT, "src"))
        self.shims = sorted(o for (d, o), (pol, _) in reg.items() if d == "euser" and pol == "shim")[:2]
        self.v2 = os.path.join(self.d, "v2")
        self.v3 = os.path.join(self.d, "v3")
        os.makedirs(self.v2)
        os.makedirs(self.v3)
        make_dso(os.path.join(self.v3, "euser.dso"), "euser", [(1, "_ZN5TSize7SetSizeEii")])

    def tearDown(self):
        shutil.rmtree(self.d, ignore_errors=True)

    def run_triage(self, ords, names, *extra):
        make_lib(os.path.join(self.v2, "euser.lib"), "euser", list(zip(ords, names)))
        app = os.path.join(self.d, "Syn.app")
        with open(app, "wb") as f:
            f.write(build_image(ords=ords))
        out = os.path.join(self.d, "out")
        p = subprocess.run([sys.executable, os.path.join(TOOLS, "triage.py"), app, "--v2-libs", self.v2, "--v3-libs", self.v3,
                            "--src", os.path.join(ROOT, "src"), "--out", out, "--no-cxxfilt"] + list(extra),
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True)
        return p, out

    def test_called_trap_needs_work(self):
        p, out = self.run_triage((9000, 9001), ("Alpha__3Foo", "Beta__3Foo"))
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertIn("imports: 2 (linker limit", p.stdout)
        self.assertIn("unresolved imports the game CALLS DIRECTLY: 1", p.stdout)
        self.assertIn("VERDICT: NEEDS WORK: 1", p.stdout)
        for name in ("Syn.triage.md", "Syn.triage.txt", os.path.join("Syn", "Syn.map.csv"), os.path.join("Syn", "Syn.imports.csv")):
            self.assertTrue(os.path.exists(os.path.join(out, name)), name)
        with open(os.path.join(out, "Syn", "Syn.guess.json")) as f:
            self.assertEqual(json.load(f)["uid3"], "abcdef01")
        self.assertEqual(self.run_triage((9000, 9001), ("Alpha__3Foo", "Beta__3Foo"), "--strict")[0].returncode, 1)

    def test_shimmed_imports_are_ready(self):
        p, _ = self.run_triage(tuple(self.shims), ("Alpha__3Foo", "Beta__3Foo"), "--strict")
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertIn("predicted: shim=2 direct=0 trap=0", p.stdout)
        self.assertIn("VERDICT: READY TO TRY", p.stdout)

    def test_errors_exit_2(self):
        app = os.path.join(self.d, "bad.app")
        with open(app, "wb") as f:
            f.write(b"x" * 200)
        base = [sys.executable, os.path.join(TOOLS, "triage.py")]
        args = ["--v2-libs", self.v2, "--v3-libs", self.v3, "--src", os.path.join(ROOT, "src"), "--out",
                os.path.join(self.d, "o"), "--no-cxxfilt"]
        p = subprocess.run(base + [app] + args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True)
        self.assertEqual(p.returncode, 2)
        self.assertIn("step0", p.stderr)
        p = subprocess.run(base + [os.path.join(self.d, "none.app")] + args, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                           universal_newlines=True)
        self.assertEqual(p.returncode, 2)
        self.assertIn("not a file", p.stderr)


if __name__ == "__main__":
    unittest.main()
