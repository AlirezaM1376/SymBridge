"""Tests for imports_join.py: a synthetic EKA1 import library (ar archive of COFF objects) and the demangler."""
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
import imports_join as IJ  # noqa: E402


def coff_member(dll, ordinal, symbol):
    secs = [(b".text", b"\x00\x00\xa0\xe1"), (b".idata$4", struct.pack("<I", 0x80000000 | ordinal)),
            (b".idata$7", dll.encode() + b".dll\0\0\0")]
    nsec = len(secs)
    body = b""
    ptrs = []
    off = 20 + 40 * nsec
    for _, data in secs:
        ptrs.append(off + len(body))
        body += data
    symptr = 20 + 40 * nsec + len(body)
    sym = struct.pack("<IIIhHBB", 0, 4, 0, 1, 0, 2, 0)        # long name at string-table offset 4, value 0, section 1, external
    strtab = struct.pack("<I", 4 + len(symbol) + 1) + symbol.encode() + b"\0"
    hdr = struct.pack("<HHIIIHH", 0x1c0, nsec, 0, symptr, 1, 0, 0)
    sh = b"".join(struct.pack("<8sIIIIIIHHI", n, 0, 0, len(d), ptrs[i], 0, 0, 0, 0, 0) for i, (n, d) in enumerate(secs))
    return hdr + sh + body + sym + strtab


def make_lib(path, dll, entries):
    out = b"!<arch>\n"
    for i, (ordinal, symbol) in enumerate(entries):
        data = coff_member(dll, ordinal, symbol)
        out += ("%-16s%-12s%-6s%-6s%-8s%-10d`\n" % ("m%d.o/" % i, "0", "0", "0", "644", len(data))).encode() + data
        out += b"\n" if len(data) & 1 else b""
    with open(path, "wb") as f:
        f.write(out)


def run(*args):
    p = subprocess.run([sys.executable, os.path.join(TOOLS, "imports_join.py")] + list(args), stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE, universal_newlines=True)
    return p.returncode, p.stdout, p.stderr


class Join(unittest.TestCase):
    def setUp(self):
        self.d = tempfile.mkdtemp(prefix="ij_")
        self.libs = os.path.join(self.d, "libs")
        os.makedirs(self.libs)
        make_lib(os.path.join(self.libs, "euser.lib"), "euser",
                 [(5, "SetSize__5TSizeii"), (7, "memcpy"), (9, "Zork__QQ")])
        self.skel = os.path.join(self.d, "skel.csv")
        with open(self.skel, "w", newline="") as f:
            f.write("index,dll,old_ordinal,readable\n0,euser,5,\n1,euser,7,\n2,euser,9,\n3,euser,11,\n4,hal,1,\n")
        self.out = os.path.join(self.d, "joined.csv")

    def tearDown(self):
        shutil.rmtree(self.d, ignore_errors=True)

    def rows(self):
        with open(self.out, newline="") as f:
            return list(csv.DictReader(f))

    def test_join_statuses(self):
        rc, out, err = run(self.skel, self.out, self.libs)
        self.assertEqual(rc, 0, out + err)
        r = self.rows()
        self.assertEqual([x["status"] for x in r],
                         ["ok", "ok_c_symbol", "ok_not_demangled", "ordinal_not_in_lib", "no_lib"])
        self.assertEqual(r[0]["readable"], "TSize::SetSize(int, int)")
        self.assertEqual(r[0]["mangled"], "SetSize__5TSizeii")
        self.assertEqual(r[1]["readable"], "memcpy")
        self.assertEqual(list(r[0]), ["index", "dll", "old_ordinal", "readable", "mangled", "status"])
        self.assertIn("5 imports: 3 resolved (1 plain C names, 1 mangled-looking names NOT demangled), 2 unresolved", out)
        self.assertIn("no lib for DLL hal", out)

    def test_strict(self):
        self.assertEqual(run(self.skel, self.out, self.libs, "--strict")[0], 1)
        with open(self.skel, "w", newline="") as f:
            f.write("index,dll,old_ordinal,readable\n0,euser,5,\n1,euser,7,\n")
        self.assertEqual(run(self.skel, self.out, self.libs, "--strict")[0], 0)

    def test_lib_file_or_folder_and_tables(self):
        tables = os.path.join(self.d, "tables")
        rc, out, _ = run(self.skel, self.out, os.path.join(self.libs, "euser.lib"), "--dump-lib-tables", tables)
        self.assertEqual(rc, 0)
        with open(os.path.join(tables, "euser.csv"), newline="") as f:
            t = list(csv.DictReader(f))
        self.assertEqual([x["ordinal"] for x in t], ["5", "7", "9"])

    def test_errors_exit_2(self):
        self.assertEqual(run(self.skel, self.out, os.path.join(self.d, "nolibs"))[0], 2)
        self.assertEqual(run(os.path.join(self.d, "none.csv"), self.out, self.libs)[0], 2)
        bad = os.path.join(self.d, "bad.csv")
        with open(bad, "w") as f:
            f.write("a,b\n1,2\n")
        rc, _, err = run(bad, self.out, self.libs)
        self.assertEqual(rc, 2)
        self.assertIn("column", err)
        self.assertEqual(run(self.skel, os.path.join(self.d, "nodir", "o.csv"), self.libs)[0], 2)

    def test_foreign_lib_is_skipped_with_a_hint(self):
        junk = os.path.join(self.libs, "junk.lib")
        with open(junk, "wb") as f:
            f.write(b"!<arch>\n" + ("%-16s%-12s%-6s%-6s%-8s%-10d`\n" % ("a.o/", "0", "0", "0", "644", 10)).encode() + b"x" * 10)
        rc, out, _ = run(self.skel, self.out, self.libs)
        self.assertEqual(rc, 0)
        self.assertIn("junk.lib: not an EKA1", out)
        self.assertEqual(self.rows()[0]["status"], "ok")


class Demangler(unittest.TestCase):
    CASES = {
        "SetSize__5TSizeii": "TSize::SetSize(int, int)",
        "StoreL__C12CEikDocumentR12CStreamStoreR17CStreamDictionary": "CEikDocument::StoreL(CStreamStore&, CStreamDictionary&) const",
        "__5TPtr8PUci": "TPtr8::TPtr8(unsigned char*, int)",
        "__11CCoeControl": "CCoeControl::CCoeControl()",
        "Create__10CFbsBitmapRC5TSize12TDisplayMode": "CFbsBitmap::Create(const TSize&, TDisplayMode)",
        "Get__3HALQ27HALData10TAttributeRi": "HAL::Get(HALData::TAttribute, int&)",
        "memcpy": "memcpy",
    }

    def test_known_names(self):
        for m, want in self.CASES.items():
            self.assertEqual(IJ.demangle(m), want, m)

    def test_unparsable_is_returned_unchanged(self):
        self.assertEqual(IJ.demangle("Zork__QQ"), "Zork__QQ")


if __name__ == "__main__":
    unittest.main()
