"""Tests for check_log.py: a device log made from a real baseline plus the lines a full log has and a baseline lacks."""
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

TOOLS = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROOT = os.path.dirname(TOOLS)
sys.path.insert(0, TOOLS)
import check_log  # noqa: E402

FACADE = ["audio: NewL -> facade 00abc123", "audio: facade 00abc123 freed"]


class CheckLog(unittest.TestCase):
    def setUp(self):
        games = os.path.join(ROOT, "games")
        self.gid = next(g for g in sorted(os.listdir(games)) if os.path.exists(os.path.join(games, g, "baseline.log")))
        self.d = tempfile.mkdtemp(prefix="cl_")
        self.base = os.path.join(games, self.gid, "baseline.log")
        with open(self.base, "r", encoding="latin-1", newline="") as f:
            self.lines = f.read().splitlines()

    def tearDown(self):
        shutil.rmtree(self.d, ignore_errors=True)

    def log(self, lines):
        p = os.path.join(self.d, "run.log")
        with open(p, "w", newline="") as f:
            f.write("\r\n".join(lines) + "\r\n")
        return p

    def good(self):
        return self.lines + ["foreground: gain=1 told AppUi override=0, observers=1"] + FACADE + ["SymBridge GUI exit"]

    def run_cli(self, *args):
        p = subprocess.run([sys.executable, "-I", os.path.join(TOOLS, "check_log.py")] + list(args),
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True)
        return p.returncode, p.stdout, p.stderr

    def failing(self, lines):
        rc, out, _ = self.run_cli(self.log(lines), self.base)
        return rc, re.findall(r"^FAIL: (\w)\)", out, re.M)

    def test_good_log_passes_and_ordinary_foreground_lines_are_fine(self):
        rc, out, err = self.run_cli(self.log(self.good()), "--game", self.gid, "--root", ROOT)
        self.assertEqual((rc, err), (0, ""), out)
        self.assertEqual(out.count("PASS:"), 10)

    def test_old_host_name_is_accepted(self):
        self.assertEqual(self.failing([l.replace("SymBridge GUI exit", "legacyhost GUI exit") for l in self.good()])[0], 0)

    def test_baseline_alone_fails_only_the_gui_exit(self):
        self.assertEqual(self.failing(self.lines + FACADE), (1, ["i"]))

    def test_each_failure(self):
        g = self.good()
        self.assertEqual(self.failing(g + ["  STOPPED at import #5: foo"]), (1, ["b"]))
        self.assertEqual(self.failing(g + ["foreground: old code left with -2 (gain=1, observer 00112233)"]), (1, ["f"]))
        self.assertEqual(self.failing(g + ["audio: REAL STREAM -> silent fake: open (-5)"]), (1, ["f"]))
        self.assertEqual(self.failing(g + ["audio: NewL -> facade 00dead00"]), (1, ["g"]))
        self.assertEqual(self.failing(g + ["hits #77 euser 5 = 3"]), (1, ["d"]))
        self.assertEqual(self.failing([l for l in g if "ended by themselves" not in l]), (1, ["e"]))
        self.assertEqual(self.failing([re.sub(r"type=0", "type=2", l) if l.startswith("CRASHWATCH") else l for l in g]), (1, ["i"]))
        self.assertEqual(self.failing([l.replace("exit=normal", "exit=crash") for l in g]), (1, ["c"]))
        other = [re.sub(r"uid3=[0-9a-f]{8}", "uid3=12345678", l) for l in g]
        self.assertIn("j", self.failing(other)[1])

    def test_slow_run_fails_the_frame_rate_check(self):
        g = [re.sub(r"(gfx: total frames \d+ in )(\d+)( ms)", lambda m: m.group(1) + str(int(m.group(2)) * 3) + m.group(3), l)
             for l in self.good()]
        self.assertEqual(self.failing(g), (1, ["h"]))

    def test_usage_and_missing_files_exit_2(self):
        self.assertEqual(self.run_cli("only_one")[0], 2)
        self.assertEqual(self.run_cli("a", "b", "--game", "x")[0], 2)
        rc, _, err = self.run_cli(os.path.join(self.d, "none.log"), self.base)
        self.assertEqual(rc, 2)
        self.assertIn("missing file", err)
        rc, _, err = self.run_cli(self.log(self.good()), "--game", "nosuchgame", "--root", ROOT)
        self.assertEqual(rc, 2)

    def test_parsers(self):
        self.assertEqual(check_log.parse_uid3(["SUMMARY uid3=0BA8F6AF imports=1"]), "0ba8f6af")
        self.assertEqual(check_log.parse_link_counts(["x link: direct=1 shim=2 trap=3 (total 6)"]), (1, 2, 3))
        self.assertIsNone(check_log.parse_total_frames_rate(["gfx: total frames 10 in 0 ms"]))


if __name__ == "__main__":
    unittest.main()
