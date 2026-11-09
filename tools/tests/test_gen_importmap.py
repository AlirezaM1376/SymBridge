"""Tests for gen_importmap.py on small synthetic maps in a scratch project root."""
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

HEADER = "dll,v2_ordinal,v2_name,status,v3_ordinal,v3_variant,v3_alt\n"


def entries(text):
    return re.findall(r"\{ (\d+), (\d+), (\d+), (\d+) \},\s+// (.*)", text)


class Gen(unittest.TestCase):
    def setUp(self):
        self.d = tempfile.mkdtemp(prefix="gi_")
        os.makedirs(os.path.join(self.d, "games", "a"))
        os.makedirs(os.path.join(self.d, "games", "b"))
        os.makedirs(os.path.join(self.d, "src"))
        os.makedirs(os.path.join(self.d, "tools"))
        self.out = os.path.join(self.d, "src", "importmap_data.cpp")

    def tearDown(self):
        shutil.rmtree(self.d, ignore_errors=True)

    def put(self, game, rows, name="map.csv", header=HEADER):
        with open(os.path.join(self.d, "games", game, name), "w", newline="") as f:
            f.write(header + "".join(r + "\n" for r in rows))

    def overlay(self, rows):
        with open(os.path.join(self.d, "tools", "importmap_curated.csv"), "w") as f:
            f.write("# comment\ndll,v2_ordinal,kind,v3_ordinal,note\n" + "".join(r + "\n" for r in rows))

    def gen(self, *extra):
        p = subprocess.run([sys.executable, "-I", os.path.join(TOOLS, "gen_importmap.py"), "--root", self.d] + list(extra),
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True)
        return p

    def text(self):
        with open(self.out, newline="") as f:
            return f.read()

    def test_kinds(self):
        self.put("a", ["euser,1,Foo__3Bar,exact,10,,",
                       "euser,2,Baz__3Bar,name_only,,,",
                       "euser,3,Qux__3Bar,ambiguous,,,",
                       "euser,4,__rt_helper,missing,,,",
                       "euser,5,Other__3Bar,missing,,,",
                       "euser,6,Gone__3Bar,no_v3_dso,,,",
                       "euser,7,Ctor__3Bar,exact,20,D1,20:D1|21:D2",
                       "euser,8,Unknown,no_v2_lib,,,"])
        p = self.gen()
        self.assertEqual(p.returncode, 0, p.stderr)
        kinds = dict((int(o), int(k)) for _, o, _, k, _ in entries(self.text()))
        self.assertEqual(kinds, {1: 0, 2: 6, 3: 7, 4: 3, 5: 1, 6: 4, 7: 5})      # 8 is skipped
        self.assertIn("imports skipped", p.stderr)
        self.assertTrue(self.text().endswith("};\n"))
        self.assertNotIn("\r", self.text())

    def test_merge_union_and_conflicts(self):
        self.put("a", ["euser,1,Foo__3Bar,exact,10,,", "euser,2,X__3Bar,no_v3_dso,,,"])
        self.put("b", ["euser,1,Foo__3Bar,exact,10,,", "euser,2,X__3Bar,exact,30,,", "euser,3,Y__3Bar,missing,,,"])
        self.assertEqual(self.gen().returncode, 0)
        self.assertEqual(len(entries(self.text())), 3)
        self.assertIn("{ 0, 2, 30, 0 }", self.text())                             # real answer beats no_v3_dso
        self.put("b", ["euser,1,Foo__3Bar,exact,11,,"])                            # disagreement
        p = self.gen()
        self.assertEqual(p.returncode, 2)
        self.assertIn("maps disagree", p.stderr)
        self.put("b", ["euser,1,Different__3Bar,exact,10,,"])                      # other generation
        p = self.gen()
        self.assertEqual(p.returncode, 2)
        self.assertIn("v2 name differs", p.stderr)

    def test_bad_input_is_listed_not_crashed(self):
        self.put("a", ["euser,1,Foo__3Bar,exact,,,", "euser,2,Foo2__3Bar,exact,0,,", "euser,3,Foo3__3Bar,weird,,,",
                       "euser,4,,exact,5,,", "euser,x,Foo5__3Bar,exact,5,,"])
        p = self.gen()
        self.assertEqual(p.returncode, 2)
        for frag in ("without v3_ordinal", "not in 1..65535", "unknown status", "no v2_name", "unreadable row"):
            self.assertIn(frag, p.stderr)
        self.assertFalse(os.path.exists(self.out))
        self.put("a", ["euser,1,Foo__3Bar,exact,5"], header="dll,v2_ordinal,status,v3_ordinal\n")
        p = self.gen()
        self.assertEqual(p.returncode, 2)
        self.assertIn("missing column", p.stderr)

    def test_overlay(self):
        self.put("a", ["euser,1,Foo__3Bar,missing,,,", "euser,2,Baz__3Bar,exact,10,,"])
        self.overlay(["euser,1,virtual_slot,,slot", "euser,2,direct,10,same as the map"])
        p = self.gen()
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertIn("{ 0, 1, 0, 2 }", self.text())
        self.assertIn("redundant", p.stderr)
        for rows, frag in ((["euser,9,virtual_slot,,x"], "no map CSV"), (["euser,1,nonsense,,x"], "unreadable"),
                           (["euser,1,direct,,x"], "needs a v3_ordinal"), (["euser,1,99,,x"], "unreadable"),
                           (["euser,1,virtual_slot,,x", "euser,1,virtual_slot,,y"], "appears twice")):
            self.overlay(rows)
            p = self.gen()
            self.assertEqual(p.returncode, 2, rows)
            self.assertIn(frag, p.stderr)
        p = self.gen("--overlay", os.path.join(self.d, "nothere.csv"))
        self.assertEqual(p.returncode, 2)
        self.assertIn("does not exist", p.stderr)

    def test_check_mode(self):
        self.put("a", ["euser,1,Foo__3Bar,exact,10,,"])
        self.assertEqual(self.gen("--check").returncode, 1)                        # file missing
        self.assertEqual(self.gen().returncode, 0)
        self.assertEqual(self.gen("--check").returncode, 0)
        crlf = self.text().replace("\n", "\r\n")
        with open(self.out, "w", newline="") as f:
            f.write(crlf)
        self.assertEqual(self.gen("--check").returncode, 0)                        # CRLF checkout is fine
        self.put("a", ["euser,1,Foo__3Bar,exact,11,,"])
        p = self.gen("--check")
        self.assertEqual(p.returncode, 1)
        self.assertIn("not up to date", p.stderr)

    def test_write_errors_and_no_maps(self):
        self.assertEqual(self.gen().returncode, 2)                                 # no CSVs at all
        self.put("a", ["euser,1,Foo__3Bar,exact,10,,"])
        p = self.gen("--out", os.path.join(self.d, "nofolder", "x.cpp"))
        self.assertEqual(p.returncode, 2)
        self.assertIn("does not exist", p.stderr)

    def test_real_repository_is_up_to_date(self):
        p = subprocess.run([sys.executable, "-I", os.path.join(TOOLS, "gen_importmap.py"), "--root", ROOT, "--check"],
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True)
        self.assertEqual(p.returncode, 0, p.stderr)


if __name__ == "__main__":
    unittest.main()
