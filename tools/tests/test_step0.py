"""Tests for step0.py on a tiny synthetic EKA1 image (no game binary needed).

The image: one function (bl thunk0 ; bx lr) exported as #1, two ARM import thunks, an IAT of two euser imports, a
relocation table, an import section. Expected: both imports found, import 0 called and reached, import 1 unused.
"""
import csv
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

TOOLS = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, TOOLS)
import step0  # noqa: E402

BASE = 0x10000000


def build_image(flags=0x3, comp=0, uid2=0x100039CE, ords=(5, 7)):
    code = bytearray()
    bl_off = (8 - (0 + 8)) // 4                      # bl at 0 -> thunk0 at 8
    code += struct.pack("<I", 0xEB000000 | (bl_off & 0xFFFFFF))
    code += struct.pack("<I", 0xE12FFF1E)             # bx lr
    iat = 40
    for k in range(2):                                # thunk k at 8 + 16k, literal at +12
        code += struct.pack("<IIII", 0xE59FC004, 0xE59CC000, 0xE12FFF1C, BASE + iat + 4 * k)
    assert len(code) == 40
    code += struct.pack("<II", *ords)                 # IAT: file holds the import ordinals
    code += struct.pack("<I", 0)                      # IAT terminator
    code += struct.pack("<I", BASE + 0)               # export table: #1 = function at 0
    cs = len(code)
    co = 0x7C
    exportoff = co + 52
    impoff = co + cs
    name = b"euser[100039e5]\0"
    imp = struct.pack("<I", 0)                        # size, patched below
    nameoff = 4 + 8 + 8
    imp += struct.pack("<II", nameoff, 2) + struct.pack("<II", *ords) + name
    imp += b"\0" * ((-len(imp)) % 4)
    imp = struct.pack("<I", len(imp)) + imp[4:]
    reloc_off = impoff + len(imp)
    rel = struct.pack("<II", 0, 0) + struct.pack("<II", 0, 8 + 2 * 2) + struct.pack("<HH", 0x3000 | 20, 0x3000 | 36)
    rel = struct.pack("<II", len(rel) - 8, 2) + rel[8:]
    hdr = bytearray(0x7C)
    u1, u2, u3 = 0x10000079, uid2, 0xABCDEF01
    struct.pack_into("<III", hdr, 0, u1, u2, u3)
    struct.pack_into("<I", hdr, 12, step0.uid_checksum(bytes(hdr[:12])))
    hdr[16:20] = b"EPOC"
    struct.pack_into("<I", hdr, 0x14, 0x2000)
    struct.pack_into("<I", hdr, 0x1C, comp)
    struct.pack_into("<I", hdr, 0x2C, flags)
    struct.pack_into("<IIIIII", hdr, 0x30, cs, 0, 0x1000, 0x100000, 0x2000, 0)
    struct.pack_into("<IIII", hdr, 0x48, 0, BASE, 0, 1)                    # entry, codebase, database, dllrefs
    struct.pack_into("<II", hdr, 0x58, exportoff, 1)
    struct.pack_into("<IIIII", hdr, 0x60, 40, co, 0, impoff, reloc_off)
    struct.pack_into("<H", hdr, 0x78, 350)
    return bytes(hdr) + bytes(code) + imp + rel


def run_step0(*args):
    p = subprocess.run([sys.executable, os.path.join(TOOLS, "step0.py")] + list(args), stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE, universal_newlines=True)
    return p.returncode, p.stdout, p.stderr


class Step0(unittest.TestCase):
    def setUp(self):
        self.d = tempfile.mkdtemp(prefix="s0_")
        self.app = os.path.join(self.d, "T.app")
        with open(self.app, "wb") as f:
            f.write(build_image())

    def tearDown(self):
        shutil.rmtree(self.d, ignore_errors=True)

    def test_analysis(self):
        js, skel = os.path.join(self.d, "o.json"), os.path.join(self.d, "s.csv")
        rc, out, err = run_step0(self.app, "--json", js, "--dump-imports", skel, "--strict")
        self.assertEqual(rc, 0, out + err)
        self.assertIn("[PASS] one thunk per import", out)
        with open(js) as f:
            j = json.load(f)
        self.assertEqual(j["uid3"], 0xABCDEF01)
        self.assertEqual(j["mode"], "arm")
        self.assertEqual(j["thunks"], {"8": 0, "24": 1})
        self.assertEqual(j["called"], [0])
        self.assertEqual(j["reach"], [0])
        self.assertEqual(j["not_found"], [1])
        self.assertTrue(all(ok for _, ok in j["checks"]))
        with open(skel, newline="") as f:
            rows = list(csv.reader(f))
        self.assertEqual(rows, [["index", "dll", "old_ordinal", "readable"], ["0", "euser", "5", ""], ["1", "euser", "7", ""]])

    def test_map_files_of_both_kinds(self):
        for col in ("readable", "v2_name"):
            ord_col = "old_ordinal" if col == "readable" else "v2_ordinal"
            p = os.path.join(self.d, col + ".csv")
            with open(p, "w", newline="") as f:
                w = csv.writer(f)
                w.writerow(["index", "dll", ord_col, col])
                w.writerow([0, "euser", 5, "User::Alloc(int)"])
                w.writerow([1, "euser", 7, "User::Free(void*)"])
            rc, out, err = run_step0(self.app, "--map", p, "--strict", "--verbose")
            self.assertEqual(rc, 0, out + err)
            self.assertIn("[PASS] import section equals the map CSV", out)
            self.assertIn("User::Alloc(int)", out)

    def test_wrong_map_fails_the_check(self):
        p = os.path.join(self.d, "m.csv")
        with open(p, "w", newline="") as f:
            f.write("index,dll,old_ordinal,readable\n0,euser,5,a\n1,euser,8,b\n")
        rc, out, _ = run_step0(self.app, "--map", p)
        self.assertEqual(rc, 0)
        self.assertIn("[FAIL] import section equals the map CSV", out)
        rc, out, _ = run_step0(self.app, "--map", p, "--strict")
        self.assertEqual(rc, 1)

    def test_strict_and_expect(self):
        self.assertEqual(run_step0(self.app, "--expect-imports", "3")[0], 0)
        self.assertEqual(run_step0(self.app, "--expect-imports", "3", "--strict")[0], 1)
        self.assertEqual(run_step0(self.app, "--expect-imports", "2", "--strict")[0], 0)

    def test_cannot_analyse_gives_exit_2_and_no_output_file(self):
        cases = {"not E32": b"x" * 200, "symbian 9": build_image(flags=0x3 | (1 << 24)),
                 "compressed": build_image(comp=0x101F7AFC), "truncated": build_image()[:150]}
        for label, data in cases.items():
            p = os.path.join(self.d, "bad.app")
            with open(p, "wb") as f:
                f.write(data)
            js = os.path.join(self.d, "bad.json")
            rc, out, err = run_step0(p, "--json", js)
            self.assertEqual(rc, 2, label + out + err)
            self.assertTrue(err.startswith("step0: "), label + err)
            self.assertFalse(os.path.exists(js), label)
        rc, _, err = run_step0(os.path.join(self.d, "missing.app"))
        self.assertEqual(rc, 2)
        self.assertEqual(run_step0(self.app, "--entry", "zz")[0], 2)
        self.assertEqual(run_step0(self.app, "--map", os.path.join(self.d, "none.csv"))[0], 2)

    def test_host_log_comparison(self):
        log = os.path.join(self.d, "h.log")
        with open(log, "w") as f:
            f.write("link: trap euser (2): 5 7 \n")
        rc, out, _ = run_step0(self.app, "--hostlog", log, "--expect-traps", "2", "--strict")
        self.assertEqual(rc, 0, out)
        self.assertIn("trap imports that the game calls directly (would be hit if executed): 1", out)
        self.assertEqual(run_step0(self.app, "--hostlog", log, "--expect-traps", "3", "--strict")[0], 1)


if __name__ == "__main__":
    unittest.main()
