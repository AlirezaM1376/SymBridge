"""Tests for mkbaseline.py, accept.py and the profile stub of newgame.py."""
import json
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
import mkbaseline  # noqa: E402
import newgame     # noqa: E402


def tool(name, *args):
    p = subprocess.run([sys.executable, os.path.join(TOOLS, name)] + list(args), stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, universal_newlines=True)
    return p.returncode, p.stdout


class MkBaseline(unittest.TestCase):
    def test_baselines_are_a_fixed_point(self):
        """reducing an already reduced log changes nothing"""
        for gid in sorted(os.listdir(os.path.join(ROOT, "games"))):
            p = os.path.join(ROOT, "games", gid, "baseline.log")
            if not os.path.exists(p):
                continue
            with open(p, "r", encoding="latin-1", newline="") as f:
                raw = f.read()
            self.assertEqual("".join(l + "\r\n" for l in mkbaseline.reduce_lines(raw.splitlines())), raw, gid)

    def test_noise_is_dropped(self):
        lines = ["audio: opened 1", "key: down 5", "link: direct=1 shim=2 trap=3 (total 6)", "SUMMARY uid3=00000001 imports=6"]
        self.assertEqual(mkbaseline.reduce_lines(lines), lines[2:])


class ProfileStub(unittest.TestCase):
    def known_keys(self):
        keys = set()
        with open(os.path.join(ROOT, "src", "profile.cpp"), "r", errors="replace") as f:
            keys |= set(re.findall(r'_L8\("([a-z_0-9]+)"\)', f.read()))
        with open(os.path.join(ROOT, "src", "hostconfig.cpp"), "r", errors="replace") as f:   # shared keys
            keys |= set(re.findall(r'_LIT8\(KKey\w+, "([a-z_0-9]+)"\)', f.read()))
        return keys

    def keys_in(self, text):
        return [m.group(1) for m in re.finditer(r"^([a-z_0-9]+)=", text, re.M)]

    def test_only_keys_the_parser_knows(self):
        known = self.known_keys()
        self.assertIn("screen_width", known)
        for guess in ({}, {"screen": [240, 320], "display_mode": 7}, {"screen": [176, 208], "display_mode": 10},
                      {"screen": [9999, 1], "display_mode": 5}):
            text = newgame.profile_stub("X", "0548e337", guess)
            for k in self.keys_in(text) + re.findall(r"^# ([a-z_0-9]+)=", text, re.M):
                self.assertIn(k, known, k)

    def test_values(self):
        t = newgame.profile_stub("X", "0548e337", {"screen": [240, 320], "display_mode": 10})
        self.assertIn("screen_width=240", t)
        self.assertIn("display_mode=10", t)
        t = newgame.profile_stub("X", "0548e337", {"screen": [9999, 1], "display_mode": 5})
        self.assertEqual(self.keys_in(t), ["stop_state_tls"])       # no screen/mode guess, only the exit-trick switch


class Accept(unittest.TestCase):
    def setUp(self):
        games = os.path.join(ROOT, "games")
        self.gid = next(g for g in sorted(os.listdir(games)) if os.path.exists(os.path.join(games, g, "baseline.log")))
        self.d = tempfile.mkdtemp(prefix="acc_")
        shutil.copytree(os.path.join(ROOT, "src"), os.path.join(self.d, "src"))
        g = os.path.join(self.d, "games", self.gid)
        os.makedirs(g)
        src = os.path.join(ROOT, "games", self.gid)
        shutil.copy(os.path.join(src, "map.csv"), g)
        # the device log is from an older host build: take the numbers of the static prediction of the current src/
        import accept
        pred, _ = accept.predicted(self.d, os.path.join(g, "map.csv"))
        with open(os.path.join(src, "baseline.log"), "r", encoding="latin-1", newline="") as f:
            text = re.sub(r"direct=\d+ shim=\d+ trap=\d+",
                          "direct=%d shim=%d trap=%d" % (pred["direct"], pred["shim"], pred["trap"]), f.read())
        with open(os.path.join(self.d, "run.log"), "w", encoding="latin-1", newline="") as f:
            f.write(text)
        self.pred = pred
        with open(os.path.join(src, "game.json")) as f:
            m = json.load(f)
        m["status"] = "not_tested"
        m["versions"] = [{"id": "main", "map": "map.csv", "notes": ""}]
        with open(os.path.join(g, "game.json"), "w") as f:
            json.dump(m, f)

    def tearDown(self):
        shutil.rmtree(self.d, ignore_errors=True)

    def test_accept_makes_a_passing_game(self):
        rc, out = tool("accept.py", self.gid, os.path.join(self.d, "run.log"), "--root", self.d)
        self.assertEqual(rc, 0, out)
        rc, out = tool("check_manifest.py", "--root", self.d)
        self.assertEqual(rc, 0, out)
        with open(os.path.join(self.d, "games", self.gid, "game.json")) as f:
            m = json.load(f)
        self.assertEqual(m["status"], "plays")
        got = m["versions"][0]["expect"]
        self.assertEqual((got["direct"], got["shim"], got["trap"]),
                         (self.pred["direct"], self.pred["shim"], self.pred["trap"]))
        rc, out = tool("accept.py", self.gid, os.path.join(self.d, "run.log"), "--root", self.d)
        self.assertEqual(rc, 1, out)             # second time: needs --update

    def test_pending_game_is_allowed_but_not_with_a_baseline_file(self):
        rc, out = tool("check_manifest.py", "--root", self.d)
        self.assertEqual(rc, 0, out)
        self.assertIn("PENDING " + self.gid + "", out)
        shutil.copy(os.path.join(self.d, "run.log"), os.path.join(self.d, "games", self.gid, "baseline.log"))
        rc, out = tool("check_manifest.py", "--root", self.d)
        self.assertEqual(rc, 1, out)

    def test_wrong_map_is_refused(self):
        p = os.path.join(self.d, "games", self.gid, "map.csv")
        with open(p, newline="") as f:
            lines = f.read().splitlines(True)
        with open(p, "w", newline="") as f:
            f.write("".join(lines[:-3]))
        rc, out = tool("accept.py", self.gid, os.path.join(self.d, "run.log"), "--root", self.d)
        self.assertEqual(rc, 1, out)
        self.assertIn("REFUSED", out)

    def test_wrong_uid3_is_refused(self):
        p = os.path.join(self.d, "games", self.gid, "game.json")
        with open(p) as f:
            m = json.load(f)
        m["uid3"] = "12345678"
        with open(p, "w") as f:
            json.dump(m, f)
        rc, out = tool("accept.py", self.gid, os.path.join(self.d, "run.log"), "--root", self.d)
        self.assertEqual(rc, 1, out)
        self.assertIn("UID3", out)


if __name__ == "__main__":
    unittest.main()
