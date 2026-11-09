"""Tests for newgame.py: a full run on a synthetic game in a scratch project root (src/ is linked from the real one)."""
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
import check_manifest                   # noqa: E402
from test_imports_join import make_lib  # noqa: E402
from test_step0 import build_image      # noqa: E402
from test_v2_to_v3 import make_dso      # noqa: E402


class NewGame(unittest.TestCase):
    def setUp(self):
        self.d = tempfile.mkdtemp(prefix="ng_")
        self.root = os.path.join(self.d, "proj")
        os.makedirs(os.path.join(self.root, "games"))
        os.symlink(os.path.join(ROOT, "src"), os.path.join(self.root, "src"))
        self.v2 = os.path.join(self.d, "v2")
        self.v3 = os.path.join(self.d, "v3")
        os.makedirs(self.v2)
        os.makedirs(self.v3)
        make_dso(os.path.join(self.v3, "euser.dso"), "euser", [(1, "_ZN5TSize7SetSizeEii")])
        make_lib(os.path.join(self.v2, "euser.lib"), "euser", [(9000, "Alpha__3Foo"), (9001, "Beta__3Foo")])
        self.app = os.path.join(self.d, "My Game.app")
        with open(self.app, "wb") as f:
            f.write(build_image(ords=(9000, 9001)))
        self.g = os.path.join(self.root, "games", "mygame")
        self.prf = os.path.join(self.root, "profiles", "ABCDEF01.prf")

    def tearDown(self):
        shutil.rmtree(self.d, ignore_errors=True)

    def run_ng(self, *extra, app=None):
        p = subprocess.run([sys.executable, "-I", os.path.join(TOOLS, "newgame.py"), app or self.app, "--v2-libs", self.v2,
                            "--v3-libs", self.v3, "--root", self.root, "--no-cxxfilt", "--id", "mygame"] + list(extra),
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True)
        return p

    def test_creates_a_pending_game_that_passes_the_check(self):
        p = self.run_ng("--name", "My Game")
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertIn("VERDICT", p.stdout)
        self.assertIn("NEXT STEPS", p.stdout)
        with open(os.path.join(self.g, "game.json")) as f:
            m = json.load(f)
        self.assertEqual((m["id"], m["uid3"], m["status"], m["profile"]), ("mygame", "abcdef01", "not_tested", "ABCDEF01.prf"))
        self.assertTrue(os.path.exists(os.path.join(self.g, "map.csv")))
        self.assertTrue(os.path.exists(self.prf))
        self.assertFalse(os.path.exists(os.path.join(self.g, "baseline.log")))
        errs, pending = [], []
        check_manifest.check_game(self.g, errs, {}, {}, pending)
        self.assertEqual(errs, [])
        self.assertEqual(pending, ["mygame"])

    def test_dry_run_writes_nothing(self):
        p = self.run_ng("--dry-run")
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertIn("DRY RUN", p.stdout)
        self.assertFalse(os.path.exists(self.g))
        self.assertFalse(os.path.exists(os.path.join(self.root, "profiles")))

    def test_existing_game_and_force(self):
        self.assertEqual(self.run_ng().returncode, 0)
        p = self.run_ng()
        self.assertEqual(p.returncode, 2)
        self.assertIn("exists already", p.stderr)
        self.assertEqual(self.run_ng("--force").returncode, 0)           # still pending: replaced
        with open(self.prf, "a") as f:
            f.write("# mine\n")
        self.assertEqual(self.run_ng("--force").returncode, 0)
        with open(self.prf) as f:
            self.assertIn("# mine", f.read())                            # profile never touched
        with open(os.path.join(self.g, "baseline.log"), "w") as f:       # accepted run: protected
            f.write("x")
        with open(os.path.join(self.g, "game.json")) as f:
            before = f.read()
        p = self.run_ng("--force")
        self.assertEqual(p.returncode, 2)
        self.assertIn("accepted run", p.stderr)
        with open(os.path.join(self.g, "game.json")) as f:
            self.assertEqual(f.read(), before)

    def test_uid3_of_another_game_is_refused(self):
        other = os.path.join(self.root, "games", "other")
        os.makedirs(other)
        with open(os.path.join(other, "game.json"), "w") as f:
            json.dump({"id": "other", "uid3": "ABCDEF01"}, f)
        p = self.run_ng()
        self.assertEqual(p.returncode, 2)
        self.assertIn("already belongs to games/other", p.stderr)
        self.assertFalse(os.path.exists(self.g))

    def test_bad_input(self):
        self.assertEqual(self.run_ng(app=os.path.join(self.d, "none.app")).returncode, 2)
        p = subprocess.run([sys.executable, "-I", os.path.join(TOOLS, "newgame.py"), self.app, "--v2-libs", self.v2,
                            "--v3-libs", os.path.join(self.d, "nolibs"), "--root", self.root],
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True)
        self.assertEqual(p.returncode, 2)
        self.assertIn("not a folder", p.stderr)
        p = self.run_ng("--id", "Bad Id")                                # the later --id wins
        self.assertEqual(p.returncode, 2)
        self.assertIn("--id", p.stderr)
        bad = os.path.join(self.d, "bad.app")
        with open(bad, "wb") as f:
            f.write(b"x" * 200)
        p = self.run_ng(app=bad)
        self.assertEqual(p.returncode, 2)
        self.assertFalse(os.path.exists(self.g))


if __name__ == "__main__":
    unittest.main()
