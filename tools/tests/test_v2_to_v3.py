"""Tests for v2_to_v3.py with a synthetic v2 import library and a synthetic v3 .dso (no SDK needed)."""
import csv
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

TOOLS = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, TOOLS)
sys.path.insert(0, os.path.join(TOOLS, "tests"))
from test_imports_join import make_lib  # noqa: E402

HAVE_CXXFILT = shutil.which("c++filt") is not None


def make_dso(path, dll, entries):
    """ELF32 with a .dynsym/.dynstr pair: symbol for ordinal n sits at stub address (n-1)*4."""
    strtab = b"\0"
    syms = struct.pack("<IIIBBH", 0, 0, 0, 0, 0, 0)
    for ordinal, name in entries:
        raw = ("%s@@%s{00010000}[100039e5].dll" % (name, dll)).encode() + b"\0"
        syms += struct.pack("<IIIBBH", len(strtab), (ordinal - 1) * 4, 0, 0x12, 0, 1)
        strtab += raw
    off_str = 52
    off_sym = off_str + len(strtab)
    off_sh = off_sym + len(syms) + (-(off_sym + len(syms)) % 4)
    sh = struct.pack("<10I", *([0] * 10))
    sh += struct.pack("<10I", 0, 11, 0, 0, off_sym, len(syms), 2, 0, 4, 16)
    sh += struct.pack("<10I", 0, 3, 0, 0, off_str, len(strtab), 0, 0, 1, 0)
    ehdr = bytearray(52)
    ehdr[:4] = b"\x7fELF"
    struct.pack_into("<I", ehdr, 0x20, off_sh)
    struct.pack_into("<HH", ehdr, 0x2E, 40, 3)
    data = bytes(ehdr) + strtab + syms + b"\0" * (off_sh - off_sym - len(syms)) + sh
    with open(path, "wb") as f:
        f.write(data)


def run(*args):
    p = subprocess.run([sys.executable, os.path.join(TOOLS, "v2_to_v3.py")] + list(args), stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE, universal_newlines=True)
    return p.returncode, p.stdout, p.stderr


class Map(unittest.TestCase):
    def setUp(self):
        self.d = tempfile.mkdtemp(prefix="v23_")
        self.v2 = os.path.join(self.d, "v2")
        self.v3 = os.path.join(self.d, "v3")
        os.makedirs(self.v2)
        os.makedirs(self.v3)
        make_lib(os.path.join(self.v2, "euser.lib"), "euser", [(5, "SetSize__5TSizeii"), (7, "memcpy"), (9, "Gone__5TSizei")])
        make_dso(os.path.join(self.v3, "euser.dso"), "euser", [(1, "_ZN5TSize7SetSizeEii"), (2, "memcpy")])
        self.skel = os.path.join(self.d, "skel.csv")
        with open(self.skel, "w", newline="") as f:
            f.write("index,dll,old_ordinal,readable\n0,euser,5,\n1,euser,7,\n2,euser,9,\n3,euser,11,\n4,hal,1,\n")
        self.out = os.path.join(self.d, "map.csv")

    def tearDown(self):
        shutil.rmtree(self.d, ignore_errors=True)

    def rows(self):
        with open(self.out, newline="") as f:
            return list(csv.DictReader(f))

    @unittest.skipUnless(HAVE_CXXFILT, "c++filt not installed")
    def test_statuses_with_cxxfilt(self):
        rc, out, err = run("--v2", self.v2, "--v3", self.v3, "--imports", self.skel, "-o", self.out)
        self.assertEqual(rc, 0, out + err)
        r = self.rows()
        self.assertEqual([x["status"] for x in r], ["exact", "exact", "missing", "v2_ord_missing", "no_v2_lib"])
        self.assertEqual((r[0]["v2_name"], r[0]["v3_ordinal"]), ("TSize::SetSize(int, int)", "1"))
        self.assertEqual((r[1]["v2_name"], r[1]["v3_ordinal"]), ("memcpy", "2"))
        self.assertEqual(list(r[0]), ["index", "dll", "v2_ordinal", "v2_name", "v3_ordinal", "v3_name", "status",
                                      "v3_alt", "v3_variant", "note"])
        self.assertIn("INCOMPLETE MAP: 2 import(s)", out)

    def test_without_cxxfilt_only_name_only(self):
        rc, out, err = run("--v2", self.v2, "--v3", self.v3, "--imports", self.skel, "-o", self.out, "--no-cxxfilt")
        self.assertEqual(rc, 0, out + err)
        r = self.rows()
        self.assertEqual(r[0]["status"], "name_only")
        self.assertEqual(r[0]["v3_ordinal"], "1")
        self.assertEqual(r[1]["status"], "exact")

    def test_require_complete(self):
        args = ("--v2", self.v2, "--v3", self.v3, "--imports", self.skel, "-o", self.out, "--no-cxxfilt")
        self.assertEqual(run(*args)[0], 0)
        self.assertEqual(run(*args, "--require-complete")[0], 1)
        with open(self.skel, "w", newline="") as f:
            f.write("index,dll,old_ordinal,readable\n0,euser,5,\n1,euser,7,\n")
        self.assertEqual(run(*args, "--require-complete")[0], 0)

    def test_dll_without_v3_is_a_note_not_an_error(self):
        make_lib(os.path.join(self.v2, "plp.lib"), "plpvariant", [(1, "Foo__3Bar")])
        with open(self.skel, "w", newline="") as f:
            f.write("index,dll,old_ordinal,readable\n0,euser,5,\n1,plpvariant,1,\n")
        rc, out, _ = run("--v2", self.v2, "--v3", self.v3, "--imports", self.skel, "-o", self.out, "--no-cxxfilt",
                         "--require-complete")
        self.assertEqual(rc, 0, out)
        self.assertEqual(self.rows()[1]["status"], "no_v3_dso")
        self.assertIn("no v3 lib/dso", out)

    def test_swapped_arguments_and_errors(self):
        rc, out, err = run("--v2", self.v3, "--v3", self.v2, "--imports", self.skel, "-o", self.out)
        self.assertEqual(rc, 2, out)
        self.assertIn("no usable", err)
        self.assertEqual(run("--v2", self.v2, "--v3", self.v3, "--imports", os.path.join(self.d, "none.csv"), "-o", self.out)[0], 2)
        self.assertEqual(run("--v2", self.v2, "--v3", self.v3, "--imports", self.skel, "-o",
                             os.path.join(self.d, "nodir", "m.csv"), "--no-cxxfilt")[0], 2)

    def test_missing_cxxfilt_warns(self):
        rc, out, _ = run("--v2", self.v2, "--v3", self.v3, "--imports", self.skel, "-o", self.out,
                         "--cxxfilt", "no-such-cxxfilt")
        self.assertEqual(rc, 0)
        self.assertIn("WARNING: no-such-cxxfilt not found", out)


if __name__ == "__main__":
    unittest.main()
