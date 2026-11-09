"""Tests for check_all.py and coverage_all.py error handling, run on a scratch copy of the project (src, games, docs)."""
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
import coverage_all  # noqa: E402

FAST = "manifest,importmap,direct,report,coverage"      # everything but the unit tests (which would run this file again)


def tool(name, *args):
    p = subprocess.run([sys.executable, "-I", os.path.join(TOOLS, name)] + list(args), stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE, universal_newlines=True)
    return p.returncode, p.stdout, p.stderr


class Scratch(unittest.TestCase):
    def setUp(self):
        self.d = tempfile.mkdtemp(prefix="ca_")
        for name in ("src", "games", "docs"):
            shutil.copytree(os.path.join(ROOT, name), os.path.join(self.d, name))
        os.makedirs(os.path.join(self.d, "tools"))
        shutil.copy(os.path.join(TOOLS, "importmap_curated.csv"), os.path.join(self.d, "tools"))
        self.sync_expectations()
        self.gid = next(g for g in sorted(os.listdir(os.path.join(self.d, "games")))
                        if os.path.exists(os.path.join(self.d, "games", g, "baseline.log")))
        self.gdir = os.path.join(self.d, "games", self.gid)

    def sync_expectations(self):
        """The accepted numbers of a game come from a device run of an older host build. These tests are about the
        checks themselves, so the scratch copy starts consistent: every game expects what the CURRENT src/ predicts
        (a shim added for one game must not turn these tests red), and COVERAGE.md is regenerated."""
        import accept
        games = os.path.join(self.d, "games")
        for g in sorted(os.listdir(games)):
            mp = os.path.join(games, g, "game.json")
            if not os.path.isfile(mp):
                continue
            with open(mp, encoding="utf-8") as f:
                m = json.load(f)
            for v in m.get("versions", []):
                if "expect" in v and v.get("map"):
                    pred, n = accept.predicted(self.d, os.path.join(games, g, v["map"]))
                    v["expect"].update(direct=pred["direct"], shim=pred["shim"], trap=pred["trap"])
                    bl = os.path.join(games, g, v.get("baseline", "baseline.log"))
                    if os.path.isfile(bl):          # the manifest check also compares expect with the log's own numbers
                        with open(bl, "r", encoding="latin-1", newline="") as f:
                            text = f.read()
                        text = re.sub(r"direct=\d+ shim=\d+ trap=\d+",
                                      "direct=%d shim=%d trap=%d" % (pred["direct"], pred["shim"], pred["trap"]), text)
                        with open(bl, "w", encoding="latin-1", newline="") as f:
                            f.write(text)
            with open(mp, "w", encoding="utf-8") as f:
                json.dump(m, f, indent=2)
        tool("coverage_all.py", "--root", self.d)

    def tearDown(self):
        shutil.rmtree(self.d, ignore_errors=True)

    def check_all(self, *extra):
        return tool("check_all.py", "--root", self.d, "--only", FAST, *extra)


class CheckAll(Scratch):
    def test_clean_copy_passes(self):
        rc, out, err = self.check_all()
        self.assertEqual(rc, 0, out + err)
        self.assertIn("PASSED", out)

    def test_broken_manifest_and_outdated_report_are_found(self):
        os.unlink(os.path.join(self.gdir, "baseline.log"))
        rc, out, _ = self.check_all()
        self.assertEqual(rc, 1)
        self.assertIn("FAIL", out)
        self.assertIn("manifest", out)

    def test_changed_map_fails_coverage_and_importmap(self):
        p = os.path.join(self.gdir, "map.csv")
        with open(p, newline="") as f:
            lines = f.read().splitlines(True)
        with open(p, "w", newline="") as f:
            f.write("".join(lines[:-3]))
        rc, out, _ = self.check_all("--game", self.gid)
        self.assertEqual(rc, 1)
        self.assertIn("coverage " + self.gid, out)

    def test_pending_game_is_skipped_not_failed(self):
        pend = os.path.join(self.d, "games", "pending")
        os.makedirs(pend)
        shutil.copy(os.path.join(self.gdir, "map.csv"), pend)
        with open(os.path.join(pend, "game.json"), "w") as f:
            json.dump({"id": "pending", "name": "P", "uid3": "12345678", "status": "not_tested", "profile": "none", "notes": "",
                       "versions": [{"id": "main", "map": "map.csv", "notes": ""}]}, f)
        rc, out, err = tool("check_all.py", "--root", self.d, "--only", "manifest,coverage")
        self.assertEqual(rc, 0, out + err)
        self.assertIn("1 skipped", out)

    def test_bad_usage_exits_2(self):
        self.assertEqual(tool("check_all.py", "--only", "nonsense")[0], 2)
        self.assertEqual(tool("check_all.py", "--only", ",")[0], 2)
        rc, _, err = tool("check_all.py", "--root", self.d, "--only", "coverage", "--game", "nosuchgame")
        self.assertEqual(rc, 2)
        self.assertIn("no such game", err)


class CoverageAllErrors(Scratch):
    def test_check_and_write(self):
        out = os.path.join(self.d, "docs", "COVERAGE.md")
        self.assertEqual(tool("coverage_all.py", "--root", self.d, "--check")[0], 0)
        with open(out, "a", newline="") as f:
            f.write("tampered\n")
        rc, _, err = tool("coverage_all.py", "--root", self.d, "--check")
        self.assertEqual(rc, 1)
        self.assertIn("not up to date", err)
        self.assertEqual(tool("coverage_all.py", "--root", self.d)[0], 0)
        self.assertEqual(tool("coverage_all.py", "--root", self.d, "--check")[0], 0)
        os.unlink(out)
        self.assertEqual(tool("coverage_all.py", "--root", self.d, "--check")[0], 1)

    def test_unreadable_game_names_the_game(self):
        with open(os.path.join(self.gdir, "game.json"), "w") as f:
            f.write("{broken")
        rc, _, err = tool("coverage_all.py", "--root", self.d)
        self.assertEqual(rc, 2)
        self.assertIn("games/" + self.gid, err)
        self.assertNotIn("Traceback", err)
        with open(os.path.join(self.gdir, "game.json"), "w") as f:
            json.dump({"name": "x", "versions": [{"id": "alt"}]}, f)
        with self.assertRaises(ValueError) as cm:
            coverage_all.load_games(self.d)
        self.assertIn("no main version", str(cm.exception))

    def test_write_error_exits_2_and_no_games_folder(self):
        rc, _, err = tool("coverage_all.py", "--root", self.d, "--out", os.path.join(self.d, "src", "importmap.cpp", "x.md"))
        self.assertEqual(rc, 2)
        self.assertNotIn("Traceback", err)
        shutil.rmtree(os.path.join(self.d, "games"))
        rc, _, err = tool("coverage_all.py", "--root", self.d)
        self.assertEqual(rc, 2)
        self.assertIn("no games folder", err)


if __name__ == "__main__":
    unittest.main()
