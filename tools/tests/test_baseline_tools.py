"""Tests for gamelog.py (runs, atomic write), mkbaseline.py, mkmanifest.py and the error paths of accept.py."""
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

TOOLS = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROOT = os.path.dirname(TOOLS)
sys.path.insert(0, TOOLS)
import accept       # noqa: E402
import gamelog      # noqa: E402
import mkmanifest   # noqa: E402


def tool(name, *args):
    p = subprocess.run([sys.executable, "-I", os.path.join(TOOLS, name)] + list(args), stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE, universal_newlines=True)
    return p.returncode, p.stdout, p.stderr


def rd(path):
    with open(path, "rb") as f:
        return f.read()


class Base(unittest.TestCase):
    def setUp(self):
        games = os.path.join(ROOT, "games")
        self.gid = next(g for g in sorted(os.listdir(games)) if os.path.exists(os.path.join(games, g, "baseline.log")))
        self.d = tempfile.mkdtemp(prefix="bl_")
        self.log = os.path.join(self.d, "run.log")
        shutil.copy(os.path.join(games, self.gid, "baseline.log"), self.log)
        with open(self.log, "r", encoding="latin-1", newline="") as f:
            self.lines = f.read().splitlines()
        # The copied device log was written by an older host build. accept.py compares the log with the static
        # prediction of the CURRENT src/, so the fixture takes the prediction's numbers (a registry edit must not
        # break these tests).
        pred, _ = accept.predicted(ROOT, os.path.join(games, self.gid, "map.csv"))
        self.lines = [re.sub(r"direct=\d+ shim=\d+ trap=\d+",
                             "direct=%d shim=%d trap=%d" % (pred["direct"], pred["shim"], pred["trap"]), l)
                      for l in self.lines]
        with open(self.log, "w", encoding="latin-1", newline="") as f:
            f.write("\r\n".join(self.lines) + "\r\n")
        self.root = os.path.join(self.d, "proj")
        g = os.path.join(self.root, "games", self.gid)
        os.makedirs(g)
        os.symlink(os.path.join(ROOT, "src"), os.path.join(self.root, "src"))
        shutil.copy(os.path.join(games, self.gid, "map.csv"), g)
        with open(os.path.join(games, self.gid, "game.json")) as f:
            m = json.load(f)
        m["status"] = "not_tested"
        m["versions"] = [{"id": "main", "map": "map.csv", "notes": ""}]
        self.manifest = os.path.join(g, "game.json")
        with open(self.manifest, "w") as f:
            json.dump(m, f)
        self.gdir = g
        self.uid3 = m["uid3"]

    def tearDown(self):
        shutil.rmtree(self.d, ignore_errors=True)

    def two_runs(self):
        p = os.path.join(self.d, "two.log")
        first = ["SymBridge GUI start", "link: direct=0 shim=0 trap=1 (total 1)", "SUMMARY uid3=00000009 imports=1 direct=0 shim=0 trap=1 reached=0 exit=normal"]
        with open(p, "w", newline="") as f:
            f.write("\r\n".join(first + ["audio: x"] + self.lines) + "\r\n")
        return p


class Runs(Base):
    def test_select_run(self):
        self.assertEqual(gamelog.select_run(["a", "SymBridge GUI start 1", "b", "SymBridge GUI start 2", "c"]),
                         (["SymBridge GUI start 2", "c"], 2))
        self.assertEqual(gamelog.select_run(["a", "b"]), (["a", "b"], 1))

    def test_write_text_is_atomic(self):
        p = os.path.join(self.d, "f.txt")
        gamelog.write_text(p, "one\r\n")
        self.assertEqual(rd(p), b"one\r\n")
        with mock.patch("os.replace", side_effect=OSError("boom")):
            with self.assertRaises(OSError):
                gamelog.write_text(p, "two")
        self.assertEqual(rd(p), b"one\r\n")
        self.assertEqual([n for n in os.listdir(self.d) if n.startswith(".tmp_")], [])


class MkBaselineCli(Base):
    def test_last_run_only_and_all_runs(self):
        two = self.two_runs()
        p = subprocess.run([sys.executable, "-I", os.path.join(TOOLS, "mkbaseline.py"), two], stdout=subprocess.PIPE,
                           stderr=subprocess.PIPE)
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertIn(b"2 runs", p.stderr)
        self.assertEqual(p.stdout, rd(self.log))                         # CRLF kept, equals the reduced single run
        rc, out, _ = tool("mkbaseline.py", two, "--all-runs")
        self.assertEqual(out.count("SUMMARY "), 2)

    def test_output_file_and_errors(self):
        o = os.path.join(self.d, "b.log")
        rc, out, err = tool("mkbaseline.py", self.log, "-o", o)
        self.assertEqual(rc, 0, err)
        self.assertEqual(rd(o), rd(self.log))
        self.assertEqual(tool("mkbaseline.py", os.path.join(self.d, "none.log"))[0], 2)
        empty = os.path.join(self.d, "e.log")
        with open(empty, "w") as f:
            f.write("audio: x\r\n")
        rc, _, err = tool("mkbaseline.py", empty)
        self.assertEqual(rc, 2)
        self.assertIn("no SUMMARY", err)
        self.assertEqual(tool("mkbaseline.py", self.log, "-o", os.path.join(self.d, "nofolder", "b.log"))[0], 2)


class MkManifestCli(Base):
    def test_refresh_keeps_hand_fields_and_status(self):
        with open(self.manifest) as f:
            m = json.load(f)
        m["notes"] = "mine"
        m["name"] = "Hand Name"
        with open(self.manifest, "w") as f:
            json.dump(m, f)
        shutil.copy(self.log, os.path.join(self.gdir, "baseline.log"))
        with open(os.path.join(self.gdir, "map.alt.csv"), "w") as f:
            f.write("x")
        rc, out, err = tool("mkmanifest.py", os.path.join(self.gdir, "baseline.log"), "--status", "menu")
        self.assertEqual(rc, 0, err)
        with open(self.manifest) as f:
            m = json.load(f)
        self.assertEqual((m["notes"], m["name"], m["status"]), ("mine", "Hand Name", "menu"))
        self.assertEqual([v["id"] for v in m["versions"]], ["main", "alt"])
        self.assertIn("expect", m["versions"][0])

    def test_uid3_mismatch_and_force(self):
        shutil.copy(self.log, os.path.join(self.gdir, "baseline.log"))
        with open(self.manifest) as f:
            m = json.load(f)
        m["uid3"] = "12345678"
        with open(self.manifest, "w") as f:
            json.dump(m, f)
        before = rd(self.manifest)
        rc, _, err = tool("mkmanifest.py", os.path.join(self.gdir, "baseline.log"))
        self.assertEqual(rc, 1)
        self.assertIn("uid3", err)
        self.assertEqual(rd(self.manifest), before)
        self.assertEqual(tool("mkmanifest.py", os.path.join(self.gdir, "baseline.log"), "--force")[0], 0)

    def test_errors(self):
        self.assertEqual(tool("mkmanifest.py", os.path.join(self.d, "none.log"))[0], 2)
        shutil.copy(self.log, os.path.join(self.gdir, "baseline.log"))
        with open(self.manifest, "w") as f:
            f.write("{not json")
        rc, _, err = tool("mkmanifest.py", os.path.join(self.gdir, "baseline.log"))
        self.assertEqual(rc, 2)
        self.assertNotIn("Traceback", err)
        bad = os.path.join(self.gdir, "bad.log")
        with open(bad, "w") as f:
            f.write("nothing\r\n")
        self.assertEqual(tool("mkmanifest.py", bad)[0], 2)


class AcceptCli(Base):
    def run_accept(self, *extra, log=None):
        return tool("accept.py", self.gid, log or self.log, "--root", self.root, *extra)

    def test_dry_run_writes_nothing(self):
        before = rd(self.manifest)
        rc, out, err = self.run_accept("--dry-run")
        self.assertEqual(rc, 0, err)
        self.assertIn("dry run", out)
        self.assertFalse(os.path.exists(os.path.join(self.gdir, "baseline.log")))
        self.assertEqual(rd(self.manifest), before)

    def test_last_run_of_a_two_run_log(self):
        rc, out, err = self.run_accept(log=self.two_runs())
        self.assertEqual(rc, 0, err)
        self.assertIn("2 runs", out)
        self.assertEqual(rd(os.path.join(self.gdir, "baseline.log")), rd(self.log))

    def test_missing_inputs_exit_2(self):
        self.assertEqual(self.run_accept(log=os.path.join(self.d, "none.log"))[0], 2)
        self.assertEqual(tool("accept.py", "nogame", self.log, "--root", self.root)[0], 2)
        os.unlink(os.path.join(self.gdir, "map.csv"))
        rc, _, err = self.run_accept()
        self.assertEqual(rc, 2)
        self.assertIn("map.csv", err)

    def test_not_a_normal_exit_is_refused(self):
        p = os.path.join(self.d, "crash.log")
        with open(p, "w", newline="") as f:
            f.write("\r\n".join(l.replace("exit=normal", "exit=crash") for l in self.lines) + "\r\n")
        rc, _, err = self.run_accept(log=p)
        self.assertEqual(rc, 1)
        self.assertIn("did not end normally", err)

    def test_failure_after_writing_restores_the_old_files(self):
        target = os.path.join(self.gdir, "baseline.log")
        with open(target, "wb") as f:
            f.write(b"OLD\r\n")
        before = rd(self.manifest)
        argv = ["accept.py", self.gid, self.log, "--root", self.root, "--update"]
        with mock.patch.object(sys, "argv", argv), \
                mock.patch.object(accept.mkmanifest, "update_manifest", side_effect=ValueError("boom")):
            with self.assertRaises(SystemExit) as cm:
                accept.main()
        self.assertEqual(cm.exception.code, 2)
        self.assertEqual(rd(target), b"OLD\r\n")
        self.assertEqual(rd(self.manifest), before)
        os.unlink(target)
        with mock.patch.object(sys, "argv", argv[:-1]), \
                mock.patch.object(accept.mkmanifest, "update_manifest", side_effect=ValueError("boom")):
            with self.assertRaises(SystemExit):
                accept.main()
        self.assertFalse(os.path.exists(target))


if __name__ == "__main__":
    unittest.main()
