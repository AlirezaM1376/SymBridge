#!/usr/bin/env python3
"""
step0.py - static analysis of one pre-Symbian-9 binary (EKA1, S60v1/v2, "ER5" E32Image): the first look at a new game.

  python step0.py GAME.app                              first look, text report on stdout
  python step0.py GAME.app --dump-imports skel.csv      also write the import list (the start of the map pipeline)
  python step0.py GAME.app --map names.csv --json out.json     full analysis for triage.py
  python step0.py GAME.app --map games/<id>/map.csv --hostlog games/<id>/baseline.log --strict

Reads  : GAME.app (never modified); optionally --map, --hostlog, --ctor-csv.
Writes : stdout (the report) and, only when asked, the files named by --dump-imports and --json.
Needs  : Python 3.8+, no third-party packages. It never needs the SDK or a device.

The report has six parts (details and an example: docs/tools/step0.md)
  1. E32 header, integrity checks, import section, IAT, exports, relocation table
  2. import thunks (ARM and Thumb), found from the IAT slot addresses
  3. vtables: which slots are the game's own code, which are inherited imports
  4. direct callers of every import (called / only in a vtable / address taken / not found)
  5. static reachability from the entry points (bl/b only)
  6. optional comparison with a host log (--hostlog)

Exit code: 0 = the analysis ran (failed checks are listed in the summary); 1 = --strict was given and a check failed;
2 = the file cannot be analysed (not an E32 image, Symbian 9+ image, compressed, damaged, unreadable): the reason is
printed on stderr and no output file is written.

Slot numbers are indices from the vtable header (offset word, rtti word), so the first function slot
(the destructor) is slot 2, as in the host code.
"""
import argparse
import collections
import csv
import datetime
import re
import struct
import sys

OK, BAD = "PASS", "FAIL"
results = []


def check(name, ok, detail=""):
    results.append((name, ok))
    print("  [%s] %s%s" % (OK if ok else BAD, name, ("  -> " + detail) if detail else ""))


def fatal(msg):
    """The file cannot be analysed: reason on stderr, exit code 2."""
    print("step0: " + msg, file=sys.stderr)
    sys.exit(2)


def info(msg):
    print("  [info] " + msg)


def warn(msg):
    print("  [WARN] " + msg)


# --------------------------------------------------------------------------- small helpers
def crc16_ccitt(data):
    c = 0
    for b in data:
        c ^= b << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) & 0xFFFF if c & 0x8000 else (c << 1) & 0xFFFF
    return c


def uid_checksum(first12):
    """Symbian UID checksum: CRC-16 of the odd bytes (high half) and of the even bytes (low half)."""
    return (crc16_ccitt(first12[1::2]) << 16) | crc16_ccitt(first12[0::2])


def sext(v, bits):
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


UID2_NAMES = {0x10000079: "dynamic DLL", 0x1000007A: "EXE", 0x100039CE: "application (.app)"}
CPU_NAMES = {0x1000: "x86", 0x2000: "ARM", 0x2001: "ARMv5", 0x4000: "M-Core"}
PRIO_NAMES = {150: "Low", 250: "Background", 350: "Foreground", 450: "High", 650: "WindowServer",
              750: "FileServer", 850: "RealTimeServer", 950: "Supervisor"}
COMP_KINDS = {0x101F7AFC: "Deflate (Huffman+LZ77)", 0x102822AA: "BytePair"}


class Ins(object):
    """One decoded instruction (only the facts the analysis needs)."""
    __slots__ = ("addr", "size", "link", "cond", "target", "lit", "ends")

    def __init__(self, addr, size):
        self.addr, self.size = addr, size
        self.link = self.cond = False
        self.target = None      # branch target (code offset) or None
        self.lit = None         # address of the literal word of a pc-relative ldr, or None
        self.ends = False       # control flow does not continue after this instruction


# --------------------------------------------------------------------------- main
def main():
    ap = argparse.ArgumentParser(
        prog="step0.py", description=__doc__.split("\n", 2)[1].strip(),
        epilog="Reads GAME.app only. Exit code 0 = analysed, 1 = --strict and a check failed, 2 = cannot analyse. "
               "Details: docs/tools/step0.md", formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("app", help="the game binary (.app / .exe / .dll in the pre-Symbian-9 E32 format)")
    ap.add_argument("--map", help="CSV with the symbol names in import order: either the 'joined' file of imports_join.py "
                                  "(column readable) or a game map.csv (column v2_name). Without it names show as '?'")
    ap.add_argument("--cd-checks", action="store_true",
                    help="self-test of this analyser: run the checks that are specific to the Captain Dynamo binary "
                         "(244 imports, 4 classes, 102 traps). Not a coverage baseline; needs that .app")
    ap.add_argument("--expect-imports", type=int, help="fail unless the import section has exactly N imports (from game.json)")
    ap.add_argument("--expect-traps", type=int, help="with --hostlog: fail unless the log reports N trap imports")
    ap.add_argument("--hostlog", help="device log or baseline.log with 'link: trap' lines (section 6)")
    ap.add_argument("--ctor-csv", help="constructL_static_imports.csv to compare against")
    ap.add_argument("--entry", help="entry offset(s), hex, relative to code start, comma separated. "
                                    "default: AppUi slot 15 if it can be identified by name, otherwise "
                                    "all exports + all program-own vtable slots")
    ap.add_argument("--mode", choices=("auto", "arm", "thumb"), default="auto",
                    help="instruction set of the code (default: auto-detect)")
    ap.add_argument("--dump-imports", metavar="CSV",
                    help="write the import list as index,dll,old_ordinal,readable (readable empty) "
                         "and carry on; imports_join.py fills 'readable', feed the result back with --map")
    ap.add_argument("--json", metavar="FILE",
                    help="write the analysis results (mode, thunks, vtables, callers, reachability) as JSON "
                         "for triage.py; nothing else changes")
    ap.add_argument("--strict", action="store_true", help="exit code 1 when any check in the report failed")
    ap.add_argument("--verbose", action="store_true", help="also list every thunk, caller, unused import and reached import")
    a = ap.parse_args()

    try:
        with open(a.app, "rb") as fh:
            d = fh.read()
    except OSError as ex:
        fatal("cannot read %s: %s" % (a.app, ex.strerror or ex))

    def u(o):
        return struct.unpack_from("<I", d, o)[0]

    def h16(o):
        return struct.unpack_from("<H", d, o)[0]

    # ====================================================================== 1. header
    print("== 1. E32 header and import section")
    if len(d) < 0x7C or d[16:20] != b"EPOC":
        fatal("%s is not an E32 image (no EPOC signature at offset 0x10)" % a.app)
    hdr = dict(uid1=u(0), uid2=u(4), uid3=u(8), uidchk=u(12), cpu=u(0x14), w18=u(0x18), comp=u(0x1C),
               tools=u(0x20), tlo=u(0x24), thi=u(0x28), flags=u(0x2C), codesz=u(0x30), datasz=u(0x34),
               heapmin=u(0x38), heapmax=u(0x3C), stack=u(0x40), bss=u(0x44), entry=u(0x48),
               codebase=u(0x4C), database=u(0x50), dllrefs=u(0x54), exportoff=u(0x58), exportcnt=u(0x5C),
               textsz=u(0x60), codeoff=u(0x64), dataoff=u(0x68), impoff=u(0x6C), codereloc=u(0x70),
               datareloc=u(0x74), prio=h16(0x78))
    fl = hdr["flags"]
    is_dll = bool(fl & 1)
    hdrfmt = (fl >> 24) & 0xF
    print("  UIDs %08x %08x %08x   compression=%x  flags=%x" % (hdr["uid1"], hdr["uid2"], hdr["uid3"],
                                                              hdr["comp"], fl))
    print("  code=%x text=%x data=%x bss=%x heap=%x..%x stack=%x  link base=%08x  DLL refs=%d" % (
        hdr["codesz"], hdr["textsz"], hdr["datasz"], hdr["bss"], hdr["heapmin"], hdr["heapmax"],
        hdr["stack"], hdr["codebase"], hdr["dllrefs"]))
    tv = hdr["tools"]
    try:
        us = (hdr["thi"] << 32) | hdr["tlo"]
        when = (datetime.datetime(1970, 1, 1) +
                datetime.timedelta(seconds=us / 1e6 - 62167219200)).strftime("%Y-%m-%d %H:%M:%S")
    except (OverflowError, ValueError):
        when = "?"
    print("  type: %s%s   UID2: %s" % (
        "DLL" if is_dll else "EXE", " (entry point not called)" if fl & 2 else "",
        UID2_NAMES.get(hdr["uid2"], "unknown")))
    print("  cpu word @0x14: 0x%x (%s)   tools version: %d.%02d(%d)   built: %s UTC   priority: %s" % (
        hdr["cpu"], CPU_NAMES.get(hdr["cpu"], "unknown"), tv & 0xFF, (tv >> 8) & 0xFF, tv >> 16, when,
        PRIO_NAMES.get(hdr["prio"], hdr["prio"])))
    print("  header words not decoded: @0x18=0x%08x  @0x20 etc. (raw values, not verified)" % hdr["w18"])
    if hdrfmt != 0:
        fatal("header format %d (flags bits 24-27): this is a Symbian 9+ image (J/V header), "
              "this script only parses the pre-9 format" % hdrfmt)
    if hdr["comp"] != 0:
        if hdr["comp"] in COMP_KINDS:
            fatal("COMPRESSED image: method %s (0x%08x). This script needs an uncompressed image: "
                  "uncompress the code/data first" % (COMP_KINDS[hdr["comp"]], hdr["comp"]))
        warn("non-zero value 0x%x at header offset 0x1c with no known meaning; continuing as uncompressed"
             % hdr["comp"])

    # --- header integrity (these are the only checks that are about the file, not about the layout)
    check("UID checksum is valid", uid_checksum(d[:12]) == hdr["uidchk"],
          "file %08x, computed %08x" % (hdr["uidchk"], uid_checksum(d[:12])))
    co, cs, ts = hdr["codeoff"], hdr["codesz"], hdr["textsz"]
    sane = (co + cs <= len(d) and ts <= cs and hdr["impoff"] < len(d) and
            (hdr["codereloc"] == 0 or hdr["codereloc"] < len(d)) and
            (hdr["exportcnt"] == 0 or (co <= hdr["exportoff"] and
                                       hdr["exportoff"] + 4 * hdr["exportcnt"] <= co + cs)))
    check("section offsets/sizes lie inside the file", sane,
          "file=%x code=%x..%x text=%x import@%x reloc@%x export@%x" % (
              len(d), co, co + cs, ts, hdr["impoff"], hdr["codereloc"], hdr["exportoff"]))
    if not sane:
        fatal("the header describes sections that lie outside the file (damaged or truncated file)")
    B = hdr["codebase"]
    code = d[co:co + cs]
    TEXT = ts          # text (code + thunks + vtables + literals + rodata); the IAT follows it
    imp = hdr["impoff"]

    def W(o):
        return struct.unpack_from("<I", code, o)[0]

    def Hc(o):
        return struct.unpack_from("<H", code, o)[0]

    # --- imports
    def cstr(o):
        e = d.index(b"\0", o)
        return d[o:e].decode("ascii", "replace")

    imp_size = u(imp)
    p = imp + 4
    flat = []                                   # (dll, ordinal) in import-table (= IAT) order
    per_dll = collections.OrderedDict()
    parse_ok = True
    for _ in range(hdr["dllrefs"]):
        if p + 8 > imp + imp_size:
            parse_ok = False
            break
        nameoff, n = u(p), u(p + 4)
        if p + 8 + 4 * n > imp + imp_size or imp + nameoff >= len(d):
            parse_ok = False
            break
        raw = cstr(imp + nameoff)
        name = re.sub(r"\[[0-9a-fA-F]+\]", "", raw).lower()      # "euser[100039e5]" -> "euser"
        if name.endswith(".dll"):
            name = name[:-4]
        ords = [u(p + 8 + 4 * k) for k in range(n)]
        per_dll[name] = ords
        flat += [(name, o) for o in ords]
        p += 8 + 4 * n
    for n_, o_ in per_dll.items():
        print("    %-24s %3d imports" % (n_, len(o_)))
    print("  total imports: %d" % len(flat))
    if a.dump_imports:
        try:
            with open(a.dump_imports, "w", encoding="utf-8", newline="") as fh:
                w = csv.writer(fh)
                w.writerow(["index", "dll", "old_ordinal", "readable"])
                for k, (dll_, ord_) in enumerate(flat):
                    w.writerow([k, dll_, ord_, ""])
        except OSError as ex:
            fatal("cannot write --dump-imports %s: %s" % (a.dump_imports, ex.strerror or ex))
        info("wrote %d imports to %s" % (len(flat), a.dump_imports))
    check("import section parses cleanly (%d DLLs)" % hdr["dllrefs"], parse_ok and len(per_dll) == hdr["dllrefs"])
    if a.cd_checks:
        check("import section parses to the expected total (244)", len(flat) == 244)
    if a.expect_imports is not None:
        check("import section has the expected total (%d)" % a.expect_imports, len(flat) == a.expect_imports)
    rows = []
    if a.map:
        try:
            with open(a.map, encoding="utf-8-sig", newline="") as fh:
                rows = list(csv.DictReader(fh))
            ord_col = "old_ordinal" if rows and "old_ordinal" in rows[0] else "v2_ordinal"
            name_col = "readable" if rows and "readable" in rows[0] else "v2_name"
            if rows and (ord_col not in rows[0] or name_col not in rows[0] or "dll" not in rows[0]):
                raise KeyError("columns dll, old_ordinal/v2_ordinal and readable/v2_name are needed, found: %s"
                               % ", ".join(rows[0]))
            same = (len(rows) == len(flat) and
                    all(r["dll"].lower() == f[0] and int(r[ord_col]) == f[1] for r, f in zip(rows, flat)))
        except (OSError, KeyError, ValueError) as ex:
            fatal("cannot use --map %s: %s" % (a.map, ex))
        check("import section equals the map CSV, row for row", same)
    readable = [r[name_col] or "?" for r in rows] if rows and len(rows) == len(flat) else ["?"] * len(flat)
    nimp = len(flat)

    # --- IAT: the loader fills it with addresses, in the file it holds the import ordinals
    ords_flat = [f[1] for f in flat]

    def iat_matches(off):
        return (off >= 0 and off + 4 * nimp <= len(code) and
                all(W(off + 4 * k) == ords_flat[k] for k in range(nimp)))

    iat_off = None
    if nimp == 0:
        iat_off = TEXT
    elif iat_matches(TEXT):
        iat_off = TEXT
    else:
        first = struct.pack("<II", *ords_flat[:2]) if nimp >= 2 else struct.pack("<I", ords_flat[0])
        pos = code.find(first, 0)
        while pos != -1:
            if pos % 4 == 0 and iat_matches(pos):
                iat_off = pos
                break
            pos = code.find(first, pos + 1)
    check("IAT found and its words equal the import ordinals", iat_off is not None,
          ("IAT at code offset 0x%x (%d words)%s" % (iat_off, nimp,
                                                     "" if iat_off == TEXT else "  [NOT at text end]"))
          if iat_off is not None else "assuming 0x%x, results below are unreliable" % TEXT)
    if iat_off is None:
        iat_off = TEXT
    IATABS = B + iat_off
    tail = iat_off + 4 * nimp
    exp_end = (hdr["exportoff"] - co) + 4 * hdr["exportcnt"]
    info("code section layout: text 0..0x%x | IAT 0x%x..0x%x | %d byte(s) gap (IAT terminator)"
         " | export table 0x%x..0x%x%s" % (TEXT, iat_off, tail, max(0, (hdr["exportoff"] - co) - tail),
                                           hdr["exportoff"] - co, exp_end,
                                           "" if exp_end == cs else "  (code size is 0x%x)" % cs))

    # --- exports (code-relative; bit 0 set = Thumb function)
    exports = []
    for k in range(hdr["exportcnt"]):
        v = W(hdr["exportoff"] - co + 4 * k)
        rel = v - B if B <= v < B + cs + 1 else v
        exports.append((k + 1, rel))

    # --- relocation table: the authoritative list of words in the code that hold addresses
    reloc = {}
    ro = hdr["codereloc"]
    if ro:
        rsize, rnum = u(ro), u(ro + 4)
        q, rend, bad = ro + 8, min(len(d), ro + 8 + rsize), False
        while q + 8 <= rend:
            page, bs = u(q), u(q + 4)
            if bs < 8 or q + bs > rend:
                bad = True
                break
            for k in range((bs - 8) // 2):
                e = h16(q + 8 + 2 * k)
                if e >> 12:
                    reloc[page + (e & 0xFFF)] = e >> 12
            q += bs
        good = all(o + 4 <= cs and (B <= W(o) <= B + cs + 1) for o in reloc)
        check("relocation table parses, count matches header, every entry is an in-image address",
              not bad and len(reloc) == rnum and good,
              "%d entries (header says %d)%s" % (len(reloc), rnum, "" if good else ", some values out of range"))
    else:
        info("no code relocation table")

    # ====================================================================== instruction set
    def arm_thunk_at(L):
        """thunk whose literal word is at L -> (start, size) or None"""
        s = L - 12
        if s >= 0 and s % 4 == 0:
            w0, w1, w2 = struct.unpack_from("<III", code, s)
            if (w0 & 0xFFFF0FFF) == 0xE59F0004:                 # ldr rN,[pc,#4]
                n = (w0 >> 12) & 15
                if w1 == (0xE5900000 | (n << 16) | (n << 12)) and w2 in (0xE12FFF10 | n, 0xE1A0F000 | n):
                    return s, 16                                 # ldr rN,[rN] ; bx rN | mov pc,rN
        s = L - 8
        if s >= 0 and s % 4 == 0:
            w0, w1 = struct.unpack_from("<II", code, s)
            if (w0 & 0xFFFF0FFF) == 0xE59F0000:                 # ldr rN,[pc,#0]
                n = (w0 >> 12) & 15
                if w1 == (0xE590F000 | (n << 16)):               # ldr pc,[rN]
                    return s, 12
        return None

    def thumb_thunk_at(L):
        s = L - 8
        if s >= 0 and s % 4 == 0:
            h0, h1, h2 = struct.unpack_from("<HHH", code, s)
            if (h0 & 0xF8FF) == 0x4801:                          # ldr rN,[pc,#4]
                n = (h0 >> 8) & 7
                if h1 == (0x6800 | (n << 3) | n) and h2 == (0x4700 | (n << 3)):
                    return s, 12                                 # ldr rN,[rN] ; bx rN ; nop ; .word
        s = L - 12
        if s >= 0 and s % 4 == 0:
            h = struct.unpack_from("<HHHHHH", code, s)           # variant that must preserve every low register:
            if (h[0] & 0xFF00) == 0xB400 and bin(h[0] & 0xFF).count("1") == 1:   # push {rN}
                n = (h[0] & 0xFF).bit_length() - 1
                if (h[1] == (0x4802 | (n << 8)) and h[2] == (0x6800 | (n << 3) | n)       # ldr rN,[pc,#8] ; ldr rN,[rN]
                        and h[3] == (0x4684 | (n << 3)) and h[4] == (0xBC00 | (1 << n))   # mov ip,rN ; pop {rN}
                        and h[5] == 0x4760):                                              # bx ip
                    return s, 16
        return None

    # every aligned word in the text that points at an IAT slot
    iat_lits = {}                                # literal word offset -> flat index
    for L in range(0, min(TEXT, len(code) - 3), 4):
        v = W(L)
        if IATABS <= v < IATABS + 4 * nimp and (v - IATABS) % 4 == 0:
            iat_lits[L] = (v - IATABS) // 4
    arm_t = {}
    thumb_t = {}
    for L, k in iat_lits.items():
        t = arm_thunk_at(L)
        if t:
            arm_t[t[0]] = (k, t[1])
        t = thumb_thunk_at(L)
        if t:
            thumb_t[t[0]] = (k, t[1])

    if a.mode != "auto":
        mode = a.mode
        how = "forced with --mode"
    elif thumb_t or arm_t:
        mode = "thumb" if len(thumb_t) > len(arm_t) else "arm"
        how = "%d Thumb-style and %d ARM-style import thunks" % (len(thumb_t), len(arm_t))
    elif exports:
        mode = "thumb" if exports[0][1] & 1 else "arm"
        how = "export #1 low bit"
    else:
        mode = "arm"
        how = "default, nothing to go on"
    mixed = bool(thumb_t) and bool(arm_t)
    print("  instruction set: %s (%s)%s" % (mode.upper(), how, "   ** MIXED ARM/Thumb thunks **" if mixed else ""))
    if mixed:
        warn("both ARM and Thumb thunks exist; code is decoded as %s only, call analysis for the other mode "
             "is incomplete" % mode.upper())
    if exports:
        print("  exports (%d): %s%s" % (len(exports), "  ".join("#%d=%x%s" % (o, v & ~1, "(T)" if v & 1 else "")
                                                             for o, v in exports[:8]),
                                        " ..." if len(exports) > 8 else ""))
    if hdr["uid2"] == 0x100039CE:
        info("UID2 says application: export #1 is NewApplication (returns the CApaApplication object); "
             "the header entry point is not used for DLLs")
    THUMB = (mode == "thumb")

    # ====================================================================== decode
    isdata = bytearray(TEXT + 8)

    def mark(o):
        for x in range(o, min(o + 4, TEXT)):
            isdata[x] = 1

    def decode(skip_words):
        for o in skip_words:
            if o < TEXT:
                mark(o)
        ins = {}
        if THUMB:
            x = 0
            while x + 2 <= TEXT:
                if isdata[x]:
                    x += 2
                    continue
                h = Hc(x)
                i = Ins(x, 2)
                if (h & 0xF800) == 0xF000 and x + 4 <= TEXT and not isdata[x + 2] and (Hc(x + 2) & 0xF800) == 0xF800:
                    off = ((h & 0x7FF) << 12) | ((Hc(x + 2) & 0x7FF) << 1)
                    i.size, i.link, i.target = 4, True, x + 4 + sext(off, 23)
                elif (h & 0xF800) == 0xE000:
                    i.target = x + 4 + 2 * sext(h & 0x7FF, 11)
                elif (h & 0xF000) == 0xD000 and ((h >> 8) & 15) < 14:
                    i.cond, i.target = True, x + 4 + 2 * sext(h & 0xFF, 8)
                elif (h & 0xF800) == 0x4800:
                    i.lit = ((x + 4) & ~3) + 4 * (h & 0xFF)
                if (h & 0xFF87) in (0x4700, 0x4687) or (h & 0xFF00) == 0xBD00:   # bx rm / mov pc,rm / pop {..,pc}
                    i.ends = True
                ins[x] = i
                x += i.size
        else:
            for x in range(0, TEXT - 3, 4):
                if isdata[x]:
                    continue
                w = W(x)
                c = w >> 28
                i = Ins(x, 4)
                if ((w >> 25) & 7) == 5 and c != 15:
                    i.link = bool((w >> 24) & 1)
                    i.cond = c != 14
                    i.target = x + 8 + 4 * sext(w & 0xFFFFFF, 24)
                if (((w >> 26) & 3) == 1 and not (w >> 25) & 1 and (w >> 24) & 1 and not (w >> 22) & 1
                        and not (w >> 21) & 1 and (w >> 20) & 1 and ((w >> 16) & 15) == 15):
                    i.lit = x + 8 + (w & 0xFFF if (w >> 23) & 1 else -(w & 0xFFF))
                if c == 14 and ((w & 0x0FFFFFF0) == 0x012FFF10 or (w & 0x0FFFF000) == 0x01A0F000
                                or ((w & 0x0E108000) == 0x08108000) or
                                (((w >> 26) & 3) == 1 and (w >> 20) & 1 and ((w >> 12) & 15) == 15)):
                    i.ends = True
                ins[x] = i
        return ins

    # pass 1 finds the literal pool words, pass 2 decodes again without ever decoding data as code
    literal = set(i.lit for i in decode(()).values() if i.lit is not None and 0 <= i.lit <= TEXT - 4)
    skip = set(literal) | {o for o in reloc if o < TEXT}
    isdata = bytearray(TEXT + 8)
    insn = decode(skip)

    # ====================================================================== 2. thunks
    print("\n== 2. Import thunks")
    cands = thumb_t if THUMB else arm_t
    if mixed:
        cands = dict(arm_t)
        cands.update(thumb_t)
    thunk = {}                                   # thunk start (code offset) -> flat index
    thunk_len = {}
    for s, (k, ln) in cands.items():
        thunk[s] = k
        thunk_len[s] = ln
    thunk_cover = set()
    for s, ln in thunk_len.items():
        thunk_cover.update(range(s, s + ln))
    by_k = collections.defaultdict(list)
    for s, k in thunk.items():
        by_k[k].append(s)
    dup = [k for k, v in by_k.items() if len(v) > 1]
    check("one thunk per import", len(by_k) == nimp and not dup,
          "%d thunks for %d imports%s%s" % (len(thunk), nimp,
                                            ", duplicates for %d imports" % len(dup) if dup else "",
                                            ", %d import(s) without a thunk" % (nimp - len(by_k))
                                            if len(by_k) != nimp else ""))
    if thunk:
        t_lo, t_hi = min(thunk), max(s + thunk_len[s] for s in thunk)
        sizes = collections.Counter(thunk_len.values())
        print("  thunk area %x..%x (%s), IAT at %x" % (
            t_lo, t_hi, ", ".join("%d x %d bytes" % (c, b) for b, c in sorted(sizes.items())), iat_off))
        gaps = (t_hi - t_lo) - sum(thunk_len.values())
        if gaps:
            info("%d byte(s) inside the thunk area that are not thunks" % gaps)
    else:
        print("  no thunks recognised, IAT at %x" % iat_off)
    # IAT slots that are used in a way we have no thunk for (e.g. code that loads the slot itself)
    inline = {}                                   # flat index -> [literal word offsets]
    for L, k in iat_lits.items():
        if L not in thunk_cover:
            inline.setdefault(k, []).append(L)
    if inline:
        info("%d import(s) have their IAT slot address in a literal pool outside any recognised thunk "
             "(inline use); they are counted as 'address taken'" % len(inline))
    no_ref = set(range(nimp)) - set(by_k) - set(inline)
    if no_ref:
        warn("%d import(s) have no thunk and no literal reference at all" % len(no_ref))
        if a.verbose:
            for k in sorted(no_ref):
                print("      %3d  %s:%d" % (k, flat[k][0], flat[k][1]))
    usable = bool(thunk) or bool(inline)

    def iname(k):
        return "%s:%d %s" % (flat[k][0], flat[k][1], readable[k])

    if a.verbose:
        for s in sorted(thunk):
            print("      thunk %05x  #%-3d %s" % (s, thunk[s], iname(thunk[s])))

    # ====================================================================== 3. vtables
    print("\n== 3. Vtables (program-own slots = what the program overrides)")
    words = {x: W(x) for x in range(0, TEXT, 4)}

    def fptr_ok(w):
        t = w & ~1
        if not (B <= t < B + TEXT):
            return False
        if mixed:
            return bool(w & 1) or (t - B) % 4 == 0
        return bool(w & 1) if THUMB else (w & 1) == 0 and (t - B) % 4 == 0

    def is_addr_word(x):
        # the relocation table is exact; without one fall back to the value test
        return (x in reloc) if reloc else (B <= words[x] < B + TEXT)

    tables = []
    x = 8
    while x < TEXT:
        if x not in literal and is_addr_word(x) and fptr_ok(words[x]):
            end = x
            while end < TEXT and end not in literal and is_addr_word(end) and fptr_ok(words[end]):
                end += 4
            n = (end - x) // 4
            h_off, h_rtti = words.get(x - 8, 1), words.get(x - 4, 1)
            neg = (h_off == 0 or (h_off & 0xFFFF0000) == 0xFFFF0000 or (h_off & 0xFFF00000) == 0xEFF00000)
            if n >= 1 and h_rtti == 0 and neg:
                tables.append((x - 8, h_off, [words[y] for y in range(x, end, 4)]))
            x = end
        else:
            x += 4

    # forwarders: stub that only adjusts "this" and jumps to an import (inherited virtual function)
    forward, site_of = {}, {}
    if usable:
        if THUMB:
            for s in range(0, TEXT - 5, 2):
                if isdata[s] or Hc(s) != 0xB500:                   # push {lr}
                    continue
                y = s + 2
                if y + 2 <= TEXT and (Hc(y) & 0xFF00) == 0x3800:   # subs r0,#n
                    y += 2
                i = insn.get(y)
                if i is None or i.target is None or i.cond or i.target not in thunk:
                    continue
                if i.link:
                    if y + 8 <= TEXT and Hc(y + 4) == 0xBC02 and Hc(y + 6) == 0x4708:   # pop {r1} ; bx r1
                        forward[s], site_of[s] = thunk[i.target], y
                else:
                    forward[s], site_of[s] = thunk[i.target], y
            for y, i in insn.items():                              # bare "b thunk" / "subs r0,#n ; b thunk"
                if i.target is not None and not i.link and not i.cond and i.target in thunk and y not in forward:
                    start = y - 2 if y >= 2 and (Hc(y - 2) & 0xFF00) == 0x3800 and not isdata[y - 2] else y
                    forward.setdefault(start, thunk[i.target])
                    site_of.setdefault(start, y)
        else:
            for y, i in insn.items():
                if i.target is not None and not i.link and not i.cond and i.target in thunk:
                    start = y
                    if y >= 4 and not isdata[y - 4] and (W(y - 4) & 0xFFFFF000) == 0xE2400000 and \
                            ((W(y - 4) >> 12) & 15) == 0 and ((W(y - 4) >> 16) & 15) == 0:   # sub r0,r0,#n
                        start = y - 4
                    forward[start] = thunk[i.target]
                    site_of[start] = y

    def classify(w):
        t = (w & ~1) - B
        if t in thunk:
            return "imp", thunk[t]
        if t in forward:
            return "fwd", forward[t]
        return "own", None

    vt_info = []
    for start, h_off, slots in tables:
        kinds = [classify(w) for w in slots]
        own = [(2 + i, (w & ~1) - B) for i, (w, kd) in enumerate(zip(slots, kinds)) if kd[0] == "own"]
        legacy = [(2 + i, (w & ~1) - B) for i, (w, kd) in enumerate(zip(slots, kinds)) if kd[0] != "imp"]
        c = collections.Counter()
        for kd in kinds:
            if kd[1] is not None:
                m = re.match(r"^~?(\w+)::", readable[kd[1]])
                if m:
                    c[m.group(1)] += 1
        vt_info.append(dict(start=start, off=h_off, slots=[(w & ~1) - B for w in slots], kinds=kinds, own=own,
                            own_legacy=legacy, base=c.most_common(1)[0][0] if c else "-"))
    print("  %d vtable(s) found" % len(vt_info))
    hidden = 0
    for v in vt_info:
        if len(v["slots"]) == 1 and not a.verbose:
            hidden += 1                  # destructor-only tables (plain CBase-style classes): --verbose lists them
            continue
        sign = v["off"] - (1 << 32) if v["off"] & 0x80000000 else v["off"]
        nimp_ = sum(1 for k in v["kinds"] if k[0] == "imp")
        nfwd = sum(1 for k in v["kinds"] if k[0] == "fwd")
        print("  vtable @%05x this-adj %d  slots %-3d inherited %d (+%d via stub)  base~%-14s own: %s" % (
            v["start"], sign, len(v["slots"]), nimp_, nfwd, v["base"],
            " ".join("[%d]=%x" % (s, t) for s, t in v["own"]) or "(none)"))
    if hidden:
        print("  (%d one-slot vtables not listed, use --verbose)" % hidden)
    if not usable:
        warn("no import thunks: every slot is shown as 'own' because inherited imports cannot be recognised")

    def own_of(*bases):
        for v in vt_info:
            if v["base"] in bases and v["off"] == 0:
                return v
        return None

    expect = [("application", ("CAknApplication", "CEikApplication"), {2, 5, 14}),
              ("document", ("CAknDocument", "CEikDocument"), {2, 11, 19}),
              ("appui", ("CAknAppUi", "CEikAppUi"), {2, 15, 19}),
              ("control", ("CCoeControl",), {2, 3})]
    for label, bases, want in (expect if a.cd_checks else []):
        v = own_of(*bases)
        got = {s for s, _ in v["own_legacy"]} if v else None      # Captain Dynamo definition: not a direct thunk
        check("%s own slots %s" % (label, sorted(want)), got == want,
              "found %s" % (sorted(got) if got is not None else "no table with base %s" % "/".join(bases)))

    by_start = {v["start"]: v for v in vt_info}
    for o, v in exports[:8]:
        e = v & ~1
        seen_, y, found = set(), e, []
        while y in insn and y not in seen_ and y < e + 0x100:
            seen_.add(y)
            i = insn[y]
            if i.lit is not None and 0 <= i.lit <= TEXT - 4 and i.lit in reloc:
                t = (W(i.lit) & ~1) - B
                if t in by_start:
                    found.append(t)
            if i.ends:
                break
            y += i.size
        for t in found:
            vv = by_start[t]
            info("export #%d (%x) stores vtable @%x (%d slots, own: %s)  <- for a .app this is the "
                 "application class" % (o, e, t, len(vv["slots"]),
                                        " ".join("[%d]=%x" % s for s in vv["own"]) or "none"))

    # ====================================================================== 4. callers
    print("\n== 4. Direct callers of imports")
    callers = collections.defaultdict(list)
    called, vt_only, taken_only, unused = set(), set(), set(), set()
    taken = set()
    in_vt = collections.Counter()
    vt_stubs = set()
    if not usable:
        warn("skipped: no import thunks or IAT references could be matched, so calls to imports cannot be told "
             "from other calls (the numbers would all be zero and meaningless)")
    else:
        branch_targets = {i.target for i in insn.values() if i.target is not None}
        vt_targets = {t for v in vt_info for t in v["slots"]}
        vt_stubs = {st for st in forward if st in vt_targets and st not in branch_targets}
        fwd_sites = {site_of[st] for st in vt_stubs}
        # also skip the push/pop/bx frame of such a stub: only the branch site matters for counting
        for addr, i in insn.items():
            if addr in thunk_cover or addr in fwd_sites:
                continue
            if i.target is not None and i.target in thunk:
                callers[thunk[i.target]].append(addr)
            elif i.lit is not None and i.lit in iat_lits and i.lit not in thunk_cover:
                callers[iat_lits[i.lit]].append(addr)                # inline "ldr rX,=IATslot"
        print("  vtable-only forwarder stubs (not counted as calls): %d" % len(vt_stubs))
        for v in vt_info:
            for w in v["slots"]:
                k = thunk.get(w, forward.get(w))
                if k is not None:
                    in_vt[k] += 1
        called = set(callers)
        vt_only = {k for k in in_vt if k not in called}
        taken = {k for L, k in iat_lits.items() if L not in thunk_cover}
        taken |= {thunk[(words[x] & ~1) - B] for x in literal if x in words and ((words[x] & ~1) - B) in thunk}
        taken |= {thunk[(words[x] & ~1) - B] for x in reloc if x in words and x not in literal
                  and ((words[x] & ~1) - B) in thunk}
        unused = set(range(nimp)) - called - set(in_vt) - taken
        print("  imports called directly: %d   only in vtables: %d   address taken (indirect call): %d   "
              "not found: %d" % (len(called), len(vt_only), len(taken - called - set(in_vt)), len(unused)))
        print("  coverage: %d of %d imports have a thunk, %d more are referenced inline"
              % (len(by_k), nimp, len(set(inline) - set(by_k))))
        if a.verbose:
            for k in sorted(called):
                print("      called:    %3d  %s   (%d site(s): %s%s)" % (
                    k, iname(k), len(callers[k]), " ".join("%x" % c for c in sorted(callers[k])[:6]),
                    " ..." if len(callers[k]) > 6 else ""))
            for k in sorted(unused):
                print("      not found: %3d  %s" % (k, iname(k)))

    # ====================================================================== 5. reachability
    print("\n== 5. Static reachability (bl/b only; vtable calls, function pointers and switch tables are not followed)")
    roots = []
    root_desc = ""
    reach_out = None                  # set below when the reachability pass runs (for --json)
    if a.entry:
        try:
            roots = [int(t, 16) & ~1 for t in a.entry.split(",") if t.strip()]
        except ValueError:
            fatal("--entry needs hex code offsets separated by commas, for example 0x58,0x1234 (got %r)" % a.entry)
        root_desc = "--entry"
    else:
        v = own_of("CAknAppUi", "CEikAppUi")
        if v:
            for s, t in v["own_legacy"]:
                if s == 15:
                    roots = [t]
                    root_desc = "AppUi slot 15"
        if not roots:
            roots = sorted({v & ~1 for _, v in exports} | {t for vv in vt_info for _, t in vv["own"]})
            root_desc = "all exports + all program-own vtable slots (no AppUi found by name)"
            if hdr["entry"] and not is_dll:
                roots.append(((hdr["entry"] - B) if B <= hdr["entry"] < B + cs else hdr["entry"]) & ~1)
    if not usable:
        warn("skipped: no import thunks or IAT references matched")
    elif not roots:
        print("  no entry found, give one with --entry 0x<code offset>")
    else:
        seen, reach, work, funcs = set(), set(), list(roots), set(roots)
        while work:
            x = work.pop()
            while x in insn and x not in seen and x not in literal and x < TEXT:
                seen.add(x)
                i = insn[x]
                if i.lit is not None and i.lit in iat_lits and i.lit not in thunk_cover:
                    reach.add(iat_lits[i.lit])
                if i.target is not None:
                    if i.target in thunk:
                        reach.add(thunk[i.target])
                    elif 0 <= i.target < TEXT and i.target not in seen:
                        work.append(i.target)
                        if i.link:
                            funcs.add(i.target)
                    if not i.link and not i.cond:
                        break                       # unconditional tail jump
                if i.ends:
                    break
                x += i.size
        reach_out = sorted(reach)
        print("  roots: %s (%d)" % (root_desc, len(roots)))
        print("  %d own functions, %d distinct imports reached (of %d)" % (len(funcs), len(reach), nimp))
        if a.ctor_csv:
            try:
                with open(a.ctor_csv, encoding="utf-8-sig", newline="") as fh:
                    ref = {(r["dll"], int(r["old_ordinal"])) for r in csv.DictReader(fh)}
            except (OSError, KeyError, ValueError) as ex:
                fatal("cannot use --ctor-csv %s: %s" % (a.ctor_csv, ex))
            got = {flat[k] for k in reach}
            check("same import set as constructL_static_imports.csv (%d)" % len(ref), got == ref,
                  "missing %d, extra %d" % (len(ref - got), len(got - ref)))
            for x in sorted(ref - got):
                print("      only in the CSV: %s:%d" % x)
            for x in sorted(got - ref):
                print("      only here:       %s:%d" % x)
        if a.verbose:
            for k in sorted(reach):
                print("      %3d  %s" % (k, iname(k)))

    # ====================================================================== 6. host log
    if a.hostlog:
        print("\n== 6. Host log")
        traps = set()
        try:
            with open(a.hostlog, encoding="utf-8", errors="replace") as fh:
                hostlines = fh.read().splitlines()
        except OSError as ex:
            fatal("cannot read --hostlog %s: %s" % (a.hostlog, ex.strerror or ex))
        for line in hostlines:
            m = re.match(r"^link: trap (\w+)(?: \(\d+\))?: ([\d ]+)\s*$", line.strip() + " ")
            if m:
                for o in m.group(2).split():
                    traps.add((m.group(1), int(o)))
        print("  host reports %d trap imports" % len(traps))
        if a.cd_checks:
            check("host reports 102 trap imports", len(traps) == 102)
        if a.expect_traps is not None:
            check("host reports %d trap imports" % a.expect_traps, len(traps) == a.expect_traps)
        known = set(flat)
        check("every trap import exists in the import table", traps <= known)
        hot = sorted(k for k in called if flat[k] in traps)
        print("  trap imports that the game calls directly (would be hit if executed): %d" % len(hot))
        for k in hot:
            print("      %3d  %s   (%d call site(s), first %x)" % (k, iname(k), len(callers[k]), callers[k][0]))
        vo = sum(1 for k in vt_only if flat[k] in traps)
        print("  trap imports that only appear in vtables (inherited, never called by the game): %d" % vo)

    # ====================================================================== --json (for triage.py)
    if a.json:
        import json
        out = dict(
            app=a.app, uid3=hdr["uid3"], cpu=hdr["cpu"], priority=hdr["prio"], tools=hdr["tools"], built=when,
            heapmin=hdr["heapmin"], heapmax=hdr["heapmax"], stack=hdr["stack"],
            mode=mode, mixed=mixed, code_file_offset=co, code_size=cs, text_size=TEXT, code_base=B,
            thunk_sizes=dict((str(b), c) for b, c in collections.Counter(thunk_len.values()).items()),
            imports=[dict(index=k, dll=flat[k][0], ordinal=flat[k][1], name=readable[k]) for k in range(nimp)],
            thunks=dict((str(s), k) for s, k in thunk.items()),
            called=sorted(called), vtable_only=sorted(vt_only), address_taken=sorted(taken), not_found=sorted(unused),
            callers=dict((str(k), sorted(v)) for k, v in callers.items()),
            reach=reach_out, reach_roots=root_desc, roots=roots,
            exports=[dict(ordinal=o, offset=v & ~1) for o, v in exports],
            vtables=[dict(start=v["start"], this_adj=v["off"] - (1 << 32) if v["off"] & 0x80000000 else v["off"],
                          base=v["base"], nslots=len(v["slots"]),
                          own=[[s, t] for s, t in v["own"]],
                          own_legacy=[[s, t] for s, t in v["own_legacy"]],
                          slot_imports=[[2 + i, k[1]] for i, k in enumerate(v["kinds"]) if k[1] is not None],
                          inherited=sum(1 for k in v["kinds"] if k[0] == "imp"),
                          via_stub=sum(1 for k in v["kinds"] if k[0] == "fwd"),
                          imports=sorted({k[1] for k in v["kinds"] if k[1] is not None}))
                     for v in vt_info],
            checks=[[n, ok] for n, ok in results])
        try:
            with open(a.json, "w", encoding="utf-8") as fh:
                json.dump(out, fh, indent=1)
        except OSError as ex:
            fatal("cannot write --json %s: %s" % (a.json, ex.strerror or ex))
        info("wrote analysis to %s" % a.json)

    # ====================================================================== summary
    print("\n== Summary")
    bad = [n for n, ok in results if not ok]
    print("  %d checks, %d passed, %d failed" % (len(results), len(results) - len(bad), len(bad)))
    for n in bad:
        print("    FAILED: " + n)
    return 1 if (bad and a.strict) else 0


if __name__ == "__main__":
    sys.exit(main())
