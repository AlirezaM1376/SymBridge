"""Tests for check_direct.py (run: python -m unittest discover -s tools/tests, or via check_all.py --only tests).

Uses the real src/directbind.cpp and src/importmap_data.cpp of the project; edited copies go to a temp folder.
"""
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
import check_direct  # noqa: E402


def run_on(directbind_text):
    """Run check_direct.py on a temp project whose directbind.cpp has the given text."""
    d = tempfile.mkdtemp(prefix="cd_test_")
    try:
        os.makedirs(os.path.join(d, "src"))
        shutil.copy(os.path.join(ROOT, "src", "importmap_data.cpp"), os.path.join(d, "src"))
        with open(os.path.join(d, "src", "directbind.cpp"), "w", newline="") as f:
            f.write(directbind_text)
        p = subprocess.run([sys.executable, os.path.join(TOOLS, "check_direct.py"), "--root", d],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, universal_newlines=True)
        return p.returncode, p.stdout
    finally:
        shutil.rmtree(d, ignore_errors=True)


def real_text():
    with open(os.path.join(ROOT, "src", "directbind.cpp"), "r", newline="") as f:
        return f.read()


class CheckDirect(unittest.TestCase):
    def test_current_file_passes(self):
        rc, out = run_on(real_text())
        self.assertEqual(rc, 0, out)

    def test_c_variadic_functions_are_flagged_for_review(self):
        rows = {("estlib", 82): (0, "sprintf"), ("estlib", 59): (0, "fprintf"), ("estlib", 180): (0, "strcmp"),
                ("euser", 467): (0, "TDes16::Format(TRefByValue<const TDesC16>, ...)")}
        errors, variadics, passed = check_direct.check(list(rows), rows)
        self.assertEqual(errors, [])
        self.assertEqual(sorted(v.split(": ")[1] for v in variadics), ["estlib #59", "estlib #82", "euser #467"])
        self.assertEqual(len(passed), 4)

    def test_commented_row_is_not_counted(self):
        text = real_text()
        n = len(check_direct.parse_direct_entries(text))
        extra = '    // { "euser", 99999, 0, 0, EShimRegDirect, 0 },\n    /* { "euser", 88888, 0, 0, EShimRegDirect, 0 }, */\n'
        marker = re.search(r"KDirectBindEntries\s*\[\s*\]\s*=\s*\{\s*", text)
        self.assertIsNotNone(marker)
        edited = text[:marker.end()] + "\n" + extra + text[marker.end():]
        self.assertEqual(len(check_direct.parse_direct_entries(edited)), n)
        rc, out = run_on(edited)
        self.assertEqual(rc, 0, out)

    def test_bad_active_row_fails(self):
        text = real_text()
        marker = re.search(r"KDirectBindEntries\s*\[\s*\]\s*=\s*\{\s*", text)
        edited = text[:marker.end()] + '\n    { "euser", 99999, 0, EShimRegDirect, "directbind", NULL },\n' + text[marker.end():]
        rc, out = run_on(edited)
        self.assertEqual(rc, 1, out)
        self.assertIn("euser #99999: no import-map row", out)

    def test_duplicate_row_fails(self):
        text = real_text()
        marker = re.search(r"KDirectBindEntries\s*\[\s*\]\s*=\s*\{\s*", text)
        edited = text[:marker.end()] + '\n    { "euser", 1, 0, EShimRegDirect, "directbind", NULL },\n' + text[marker.end():]
        rc, out = run_on(edited)
        self.assertEqual(rc, 1, out)
        self.assertIn("listed twice", out)

    def test_unsafe_functions_are_refused(self):
        errs, _, _ = check_direct.check([("euser", 1)], {("euser", 1): (0, "Foo(double)")})
        self.assertIn("by-value ABI-sensitive parameter: double", errs[0])
        errs, _, _ = check_direct.check([("euser", 1)], {("euser", 1): (0, "Bar(int, void (*)(int))")})
        self.assertIn("function-pointer parameter", errs[0])
        errs, _, _ = check_direct.check([("estlib", 2)], {("estlib", 2): (0, "qsort")})
        self.assertIn("deny-listed", errs[0])
        errs, _, ok = check_direct.check([("euser", 1)], {("euser", 1): (0, "Baz(const double&, int*)")})
        self.assertEqual((errs, len(ok)), ([], 1))                    # by reference or pointer is fine
        errs, var, _ = check_direct.check([("euser", 1)], {("euser", 1): (0, "Fmt(TRefByValue<const TDesC16>, ...)")})
        self.assertEqual((errs, len(var)), ([], 1))
        errs, _, _ = check_direct.check([("euser", 1)], {("euser", 1): (5, "Dtor()")})
        self.assertIn("not EDirect", errs[0])

    def test_damaged_table_fails_and_missing_file_cannot_run(self):
        rc, out = run_on("// no table here\n")
        self.assertEqual(rc, 1, out)
        self.assertIn("KDirectBindEntries", out)
        d = tempfile.mkdtemp(prefix="cd_test_")
        try:
            p = subprocess.run([sys.executable, "-I", os.path.join(TOOLS, "check_direct.py"), "--root", d],
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True)
            self.assertEqual(p.returncode, 2)
            self.assertIn("missing file", p.stderr)
        finally:
            shutil.rmtree(d, ignore_errors=True)

    def test_verbose_lists_passed_rows(self):
        p = subprocess.run([sys.executable, "-I", os.path.join(TOOLS, "check_direct.py"), "--root", ROOT, "-v"],
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True)
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertIn("ok   euser #1: memset", p.stdout)

    def test_comment_markers_inside_strings_survive(self):
        src = 'const char* a = "x // y"; // gone\n/* gone */ int b;'
        out = check_direct.strip_comments(src)
        self.assertIn('"x // y"', out)
        self.assertNotIn("gone", out)


if __name__ == "__main__":
    unittest.main()
