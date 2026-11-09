"""Tests for import_coverage.py: source reading, the shim / direct / trap rule, CSV errors and the report on a tiny project."""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

TOOLS = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, TOOLS)
import import_coverage as IC  # noqa: E402

HEAD = "index,dll,v2_ordinal,v2_name,v3_ordinal,v3_name,status,v3_alt,v3_variant,note\n"


class Source(unittest.TestCase):
    def test_strip_comments_keeps_strings_and_lines(self):
        t = 'a "x // y" /* c1\nc2 */ b // tail\n#if 0\nhidden\n#endif\nz'
        out = IC.strip_comments(t)
        self.assertIn('"x // y"', out)
        self.assertNotIn("c1", out)
        self.assertNotIn("tail", out)
        self.assertNotIn("hidden", out)
        self.assertEqual(out.count("\n"), t.count("\n"))

    def test_split_and_integer(self):
        self.assertEqual(IC.split_cpp_fields('"a,b", 5, F(1,2), {1,2}'), ['"a,b"', "5", "F(1,2)", "{1,2}"])
        self.assertEqual((IC.cpp_integer("0x10"), IC.cpp_integer("7u")), (16, 7))

    def test_classify_rule(self):
        reg = {("euser", 1): ("shim", "t"), ("euser", 2): ("direct", "t"), ("euser", 3): ("direct", "t"),
               ("euser", 4): ("direct", "t"), ("euser", 5): ("runtime", "t")}
        base = {("euser", 2): dict(v3=10, kind=0, name="f"), ("euser", 3): dict(v3=0, kind=0, name="g"),
                ("euser", 4): dict(v3=11, kind=5, name="h")}
        want = {1: "shim", 2: "direct", 3: "trap", 4: "trap", 5: "shim", 6: "trap"}
        for o, k in want.items():
            self.assertEqual(IC.classify("euser", o, reg, base), k, o)


class Project(unittest.TestCase):
    def setUp(self):
        self.d = tempfile.mkdtemp(prefix="ic_")
        self.src = os.path.join(self.d, "src")
        os.makedirs(self.src)
        self.write("shims.cpp", 'static const TShimRegEntry KT[] =\n  {\n'
                                '  { "euser", 1, 0, 0, "o", NULL },   // shim\n'
                                '  { "euser", 2, 0, EShimRegDirect, "directbind", NULL },\n'
                                '  // { "euser", 99, 0, 0, "o", NULL },\n  };\n')
        self.write("iatfill.h", "const TInt KMaxLinkImports = 3;\n")
        self.write("importmap_data.cpp", 'const char* const KImportMapDllNames[] =\n    {\n    "euser",\n    };\n'
                                         'const TImportMapEntry KImportMap[] =\n    {\n'
                                         '    { 0, 2, 10, 0 },   // F\n    };\n')
        self.gdir = os.path.join(self.d, "games", "g")
        os.makedirs(self.gdir)

    def tearDown(self):
        shutil.rmtree(self.d, ignore_errors=True)

    def write(self, name, text):
        with open(os.path.join(self.src, name), "w", newline="") as f:
            f.write(text)

    def csv(self, rows, name="map.csv", header=HEAD):
        p = os.path.join(self.gdir, name)
        with open(p, "w", newline="") as f:
            f.write(header + "".join(r + "\n" for r in rows))
        return p

    def run_ic(self, *args):
        return subprocess.run([sys.executable, "-I", os.path.join(TOOLS, "import_coverage.py"), "--src", self.src] + list(args),
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True)

    ROWS = ["0,euser,1,A(),5,A(),exact,,,", "1,euser,2,F(),10,F(),exact,,,", "2,euser,3,G(),,,missing,,,"]

    def test_registry_reading(self):
        reg, tables = IC.load_shim_registry(self.src)
        self.assertEqual(sorted(reg), [("euser", 1), ("euser", 2)])          # the commented row is not read
        self.assertEqual(tables[0][2], 2)
        self.write("dup.cpp", 'static const TShimRegEntry KU[] = { { "euser", 1, 0, 0, "o", NULL }, };')
        with self.assertRaises(ValueError):
            IC.load_shim_registry(self.src)
        os.unlink(os.path.join(self.src, "dup.cpp"))
        self.write("bad.cpp", 'static const TShimRegEntry KB[] = { { "euser", 7 }, };')
        with self.assertRaises(ValueError):
            IC.load_shim_registry(self.src)

    def test_report_and_expect(self):
        p = self.csv(self.ROWS)
        r = self.run_ic("--csv", p, "--expect", "shim=1,direct=1,trap=1")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("Linker outcome: shim=1  direct=1  trap=1", r.stdout)
        self.assertIn("**MATCH**", r.stdout)
        self.assertIn("Not exact, and no shim", r.stdout)
        r = self.run_ic("--csv", p, "--expect", "shim=3,direct=0,trap=0")
        self.assertEqual(r.returncode, 1)
        self.assertIn("**MISMATCH**", r.stdout)
        r = self.run_ic("--csv", p)
        self.assertEqual(r.returncode, 0)
        self.assertIn("Not compared", r.stdout)

    def test_expect_from_game_json_and_out_file(self):
        p = self.csv(self.ROWS)
        with open(os.path.join(self.gdir, "game.json"), "w") as f:
            json.dump({"versions": [{"id": "main", "map": "map.csv", "expect": {"shim": 1, "direct": 1, "trap": 1}}]}, f)
        out = os.path.join(self.d, "r.md")
        r = self.run_ic("--csv", p, "--out", out)
        self.assertEqual(r.returncode, 0, r.stderr)
        with open(out, encoding="utf-8") as f:
            self.assertIn("MATCH", f.read())

    def test_blocker_and_incomplete_warnings(self):
        rows = self.ROWS + ["3,euser,4,,,,no_v2_lib,,,"]
        r = self.run_ic("--csv", self.csv(rows))
        self.assertIn("BLOCKER", r.stdout)                                   # 4 imports > KMaxLinkImports 3
        self.assertIn("map is incomplete", r.stdout)

    def test_several_games_rank_shared_traps(self):
        a = self.csv(self.ROWS, "map.csv")
        b = self.csv(["0,euser,3,G(),,,missing,,,"], "map.alt.csv")
        r = self.run_ic("--csv", a, "--csv", b)
        self.assertIn("needed by several games", r.stdout)
        self.assertIn("| 2 | euser | 3 | G() |", r.stdout)

    def test_errors_exit_2(self):
        p = self.csv(self.ROWS)
        self.assertEqual(self.run_ic("--csv", p, "--expect", "nonsense").returncode, 2)
        r = self.run_ic("--csv", os.path.join(self.d, "none.csv"))
        self.assertEqual(r.returncode, 2)
        self.assertNotIn("Traceback", r.stderr)
        r = self.run_ic("--csv", self.csv(["1,euser,2"], header="index,dll,v2_ordinal\n"))
        self.assertEqual(r.returncode, 2)
        self.assertIn("missing column", r.stderr)
        r = self.run_ic("--csv", self.csv(["0,euser,x,A(),5,A(),exact,,,"]))
        self.assertEqual(r.returncode, 2)
        self.assertIn("bad dll or v2_ordinal", r.stderr)
        os.unlink(os.path.join(self.src, "iatfill.h"))
        self.assertEqual(self.run_ic("--csv", p).returncode, 2)
        shutil.rmtree(self.src)
        r = self.run_ic("--csv", p)
        self.assertEqual(r.returncode, 2)
        self.assertIn("not a folder", r.stderr)


if __name__ == "__main__":
    unittest.main()
