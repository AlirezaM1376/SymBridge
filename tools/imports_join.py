#!/usr/bin/env python3
"""
imports_join.py - fill in the names of an import skeleton from the SDK's old import libraries.

  python imports_join.py skeleton.csv joined.csv LIB_DIR_OR_LIB [LIB_DIR_OR_LIB ...] [--dump-lib-tables DIR] [--strict]

Step 2 of the map pipeline: step0.py --dump-imports writes skeleton.csv (dll, old ordinal, no name); this script looks
every (dll, ordinal) up in the S60v2 EKA1 `.lib` files and writes the symbol name next to it; v2_to_v3.py then maps the names.

Reads  : the skeleton CSV (columns index, dll, old_ordinal) and the .lib files (a folder is searched for *.lib).
Writes : joined.csv with the columns index, dll, old_ordinal, readable, mangled, status; and, with --dump-lib-tables,
         one <dll>.csv per lib (ordinal, mangled, readable) for looking things up by hand.
status : ok | ok_c_symbol (plain C name) | ok_not_demangled (mangled-looking, kept as is) | no_lib (no lib for that DLL)
         | ordinal_not_in_lib (the lib has no such ordinal: it is from another SDK/edition than the game was built with)
Exit   : 0 = joined.csv written (unresolved imports are listed, not an error); 1 = --strict and something is not resolved
         or not demangled; 2 = cannot run (unreadable skeleton, no usable lib, cannot write).

How a lib is read: ordinal = .idata$4 word & 0x7fffffff of every thunk member, symbol = its code symbol, DLL name =
.idata$7 of the tail member. Old GCC 2.9x (GNU v2) names are demangled by the demangler below (modern c++filt cannot
do this style); names it cannot parse are kept mangled. Details: docs/tools/imports_join.md
"""
import argparse, csv, glob, os, re, struct, sys, collections

# ------------------------------------------------------------------ ar / COFF
def ar_members(path):
    with open(path, "rb") as fh:
        d = fh.read()
    if d[:8] != b"!<arch>\n":
        raise ValueError("not an ar archive: " + path)
    p, out = 8, []
    while p + 60 <= len(d):
        h = d[p:p + 60]
        name, size = h[:16].decode("latin1").rstrip(), int(h[48:58])
        p += 60
        out.append((name, d[p:p + size]))
        p += size + (size & 1)
    return out

def coff(b):
    _, ns, _, sp, nsym, opt, _ = struct.unpack_from("<HHIIIHH", b, 0)
    secs = []
    for i in range(ns):
        nm, _, _, rs, rp = struct.unpack_from("<8sIIII", b, 20 + opt + 40 * i)
        secs.append((nm.rstrip(b"\0").decode("latin1"), b[rp:rp + rs] if rp else b""))
    st, syms, i = sp + 18 * nsym, [], 0
    while i < nsym:
        e = b[sp + 18 * i:sp + 18 * i + 18]
        if e[:4] == b"\0\0\0\0":
            o = struct.unpack_from("<I", e, 4)[0]
            nm = b[st + o:b.index(b"\0", st + o)].decode("latin1")
        else:
            nm = e[:8].rstrip(b"\0").decode("latin1")
        _, sec, _, cls, aux = struct.unpack_from("<IhHBB", e, 8)
        syms.append((nm, sec, cls))
        i += 1 + aux
    return secs, syms

def lib_table(path):
    dll, tab = None, {}
    for n, b in ar_members(path):
        if n in ("/", "//"):
            continue
        secs, syms = coff(b)
        sd = dict(secs)
        s7 = sd.get(".idata$7", b"")
        if dll is None and s7[:1].isalpha():
            dll = re.sub(r"\[[0-9a-fA-F]+\]", "", s7.split(b"\0")[0].decode("latin1")).lower()
            dll = dll[:-4] if dll.endswith(".dll") else dll
        d4 = sd.get(".idata$4", b"")
        if len(d4) == 4 and secs and secs[0][0] == ".text" and secs[0][1]:
            v = struct.unpack("<I", d4)[0]
            if v & 0x80000000:
                # exported symbol = external symbol defined in .text. COFF storage class is 150
                # (Thumb external) in thumb libs but 2 (plain external) in armi/arm4 libs.
                names = [s[0] for s in syms if s[1] == 1 and s[2] in (2, 150) and not s[0].startswith(".")]
                if names:
                    tab[v & 0x7FFFFFFF] = names[0]
    return dll or os.path.splitext(os.path.basename(path))[0].lower(), tab

# ------------------------------------------------------------------ GNU v2 demangler
class Bad(Exception):
    pass

OPS = {"nw": "new", "dl": "delete", "vn": "new[]", "vd": "delete[]", "as": "=", "eq": "==", "ne": "!=",
       "pl": "+", "mi": "-", "ml": "*", "dv": "/", "md": "%", "ls": "<<", "rs": ">>", "lt": "<", "gt": ">",
       "le": "<=", "ge": ">=", "aa": "&&", "oo": "||", "nt": "!", "co": "~", "pp": "++", "mm": "--",
       "ad": "&", "or": "|", "er": "^", "rf": "->", "cl": "()", "vc": "[]", "cm": ",", "apl": "+=",
       "ami": "-=", "aml": "*=", "adv": "/=", "amd": "%=", "als": "<<=", "ars": ">>=", "aad": "&=",
       "aor": "|=", "aer": "^=", "rm": "->*", "cn": "?:"}
BASIC = {"i": "int", "l": "long", "s": "short", "c": "char", "b": "bool", "f": "float", "d": "double",
         "r": "long double", "v": "void", "x": "long long", "w": "wchar_t", "e": "..."}

class Dm(object):
    def __init__(self, s):
        self.s, self.i, self.types = s, 0, []
        self.base = 1        # T/N indexes: 1-based for members (class is 0), 0-based for free functions
    def peek(self):
        return self.s[self.i] if self.i < len(self.s) else ""
    def take(self):
        c = self.peek()
        if not c:
            raise Bad()
        self.i += 1
        return c
    def num(self):
        j = self.i
        while self.peek().isdigit():
            self.i += 1
        if j == self.i:
            raise Bad()
        return int(self.s[j:self.i])
    def ident(self):
        n = self.num()
        if self.i + n > len(self.s):
            raise Bad()
        r = self.s[self.i:self.i + n]
        self.i += n
        return r
    def cls(self):
        c = self.peek()
        if c == "Q":
            self.i += 1
            if self.peek() == "_":
                self.i += 1
                n = self.num()
                if self.take() != "_":
                    raise Bad()
            else:
                n = int(self.take())
            return "::".join(self.ident() for _ in range(n))
        if c == "t":
            self.i += 1
            name = self.ident()
            n = self.num()
            args = []
            for _ in range(n):
                if self.peek() == "Z":
                    self.i += 1
                    args.append(self.one())
                else:                                   # value argument: <type><number>, e.g. i256
                    self.one()
                    args.append(str(self.num()))
            return "%s<%s>" % (name, ", ".join(args))
        if c.isdigit():
            return self.ident()
        raise Bad()
    def one(self):
        c = self.take()
        if c == "C" or c == "V":
            t = self.one()
            q = "const" if c == "C" else "volatile"
            return t + " " + q if t.endswith(("*", "&")) else q + " " + t
        if c == "U":
            return "unsigned " + self.one()
        if c == "S":
            return "signed " + self.one()
        if c == "P":
            if self.peek() == "F":
                self.i += 1
                a, r = self.func()
                return "%s (*)(%s)" % (r, a)
            return self.one() + "*"
        if c == "R":
            return self.one() + "&"
        if c == "A":
            n = self.num()
            if self.take() != "_":
                raise Bad()
            return "%s[%d]" % (self.one(), n)
        if c == "T":
            n = self.index() - self.base
            if not 0 <= n < len(self.types):
                raise Bad()
            return self.types[n]
        if c == "G":
            return self.cls()
        if c in BASIC:
            return BASIC[c]
        self.i -= 1
        if c.isdigit() or c in "Qt":
            return self.cls()
        raise Bad()
    def index(self):
        """back-reference index: one digit, or several digits closed by '_' (T11_)"""
        j = self.i
        while self.peek().isdigit():
            self.i += 1
        digits = self.s[j:self.i]
        if not digits:
            raise Bad()
        if self.peek() == "_" and len(digits) > 1:
            self.i += 1
            return int(digits)
        self.i = j + 1
        return int(digits[0])
    def func(self):
        args = []
        while self.peek() != "_":
            args.append(self.one())
        self.i += 1
        return ", ".join(args), self.one()
    def args(self):
        out = []
        while self.i < len(self.s):
            if self.peek() == "N":
                self.i += 1
                cnt = int(self.take())
                idx = self.index()
                idx -= self.base
                if not 0 <= idx < len(self.types):
                    raise Bad()
                for _ in range(cnt):
                    self.types.append(self.types[idx])
                    out.append(self.types[-1])
                continue
            t = self.one()
            self.types.append(t)
            out.append(t)
        return [a for a in out if a != "void" or len(out) > 1]

def _tail(rest, name, kind):
    """rest = text after the first '__' of 'name__[C]<class><args>'"""
    d = Dm(rest)
    const = False
    if d.peek() == "C":
        const = True
        d.i += 1
    if d.peek() == "F" and kind == "fn":                  # free function: name__F<args>
        d.i += 1
        d.base = 0
        return "%s(%s)" % (name, ", ".join(d.args()))
    c = d.cls()
    a = d.args()
    short = re.sub(r"<.*>$", "", c.split("::")[-1]) if "<" not in c.split("::")[-1][:0] else c
    short = re.sub(r"<.*>$", "", short)
    if kind == "ctor":
        name = short
    elif kind == "dtor":
        name = "~" + short
    return "%s::%s(%s)%s" % (c, name, ", ".join(a), " const" if const else "")

def demangle(m):
    try:
        for pre, kind in (("_._", "dtor"), ("_$_", "dtor")):
            if m.startswith(pre):
                d = Dm(m[len(pre):])
                c = d.cls()
                return "%s::~%s()" % (c, re.sub(r"<.*>$", "", c.split("::")[-1]))
        for pre in ("__vt_", "_vt$", "_vt."):
            if m.startswith(pre):
                return "vtable for " + Dm(m[len(pre):]).cls()
        mo = re.match(r"^__([a-z]{2,3})__(.*)$", m)
        if mo and mo.group(1) in OPS:
            return _tail(mo.group(2), "operator" + (" " if OPS[mo.group(1)][0].isalpha() else "") + OPS[mo.group(1)], "fn")
        if m.startswith("__") and (m[2:3].isdigit() or m[2:3] == "Q" or (m[2:3] == "t" and m[3:4].isdigit())):
            return _tail(m[2:], "", "ctor")
        pos = m.find("__", 1)
        while pos != -1:
            try:
                return _tail(m[pos + 2:], m[:pos], "fn")
            except (Bad, ValueError, IndexError, KeyError):     # a malformed name must never stop the whole join
                pos = m.find("__", pos + 1)
    except (Bad, ValueError, IndexError, KeyError):
        pass
    return m

# ------------------------------------------------------------------ join
def fatal(msg):
    print("imports_join: " + msg, file=sys.stderr)
    sys.exit(2)


def main():
    ap = argparse.ArgumentParser(prog="imports_join.py", description=__doc__.split("\n", 2)[1].strip(),
                                 epilog="Exit code 0 = written, 1 = --strict and something unresolved, 2 = cannot run. "
                                        "Details: docs/tools/imports_join.md")
    ap.add_argument("skeleton", help="CSV from step0.py --dump-imports (columns index, dll, old_ordinal)")
    ap.add_argument("out", help="the joined CSV to write")
    ap.add_argument("libs", nargs="+", metavar="LIB_DIR_OR_LIB", help="S60v2 .lib files or folders that contain them")
    ap.add_argument("--dump-lib-tables", metavar="DIR", help="also write one <dll>.csv (ordinal, mangled, readable) per lib")
    ap.add_argument("--strict", action="store_true", help="exit 1 when an import is unresolved or not demangled")
    a = ap.parse_args()
    skel, out, srcs, dump = a.skeleton, a.out, a.libs, a.dump_lib_tables

    libs = []
    for s in srcs:
        if not os.path.exists(s):
            print("skip %s: no such file or folder" % s)
        elif os.path.isdir(s):
            libs += sorted(glob.glob(os.path.join(s, "*.lib")))
        else:
            libs.append(s)
    tables = {}
    for p in libs:
        try:
            dll, tab = lib_table(p)
        except struct.error:
            print("skip %s: not an EKA1 (S60v2) import library; the S60v3 libs are not used by this script" % p)
            continue
        except Exception as e:
            print("skip %s: %s" % (p, e))
            continue
        if dll in tables:
            print("note: %s already loaded, %s merged into it" % (dll, p))
        tables.setdefault(dll, {}).update(tab)
        print("lib %-28s dll=%-14s %4d ordinals (max %d)" % (os.path.basename(p), dll, len(tab), max(tab) if tab else 0))
        if dump:
            try:
                os.makedirs(dump, exist_ok=True)
                with open(os.path.join(dump, dll + ".csv"), "w", newline="") as fh:
                    w = csv.writer(fh)
                    w.writerow(["ordinal", "mangled", "readable"])
                    for o, n in sorted(tab.items()):
                        w.writerow([o, n, demangle(n)])
            except OSError as e:
                fatal("cannot write the lib tables to %s: %s" % (dump, e.strerror or e))
    if not tables:
        fatal("no usable .lib file found in: %s" % " ".join(srcs))
    try:
        with open(skel, encoding="utf-8-sig", newline="") as fh:
            rows = list(csv.DictReader(fh))
        if not rows:
            raise ValueError("the skeleton has no rows")
        for need in ("index", "dll", "old_ordinal"):
            if need not in rows[0]:
                raise ValueError("column %r missing (found: %s)" % (need, ", ".join(rows[0])))
        rows = [(r["index"], r["dll"], int(r["old_ordinal"])) for r in rows]
    except (OSError, ValueError) as e:
        fatal("cannot use the skeleton %s: %s" % (skel, e))
    miss_lib, miss_ord, ok, undem, nc = collections.Counter(), collections.defaultdict(list), 0, 0, 0
    try:
        with open(out, "w", encoding="utf-8", newline="") as fh:
            w = csv.writer(fh)
            w.writerow(["index", "dll", "old_ordinal", "readable", "mangled", "status"])
            for idx, dll, o in rows:
                if dll not in tables:
                    st, m, rd = "no_lib", "", ""
                    miss_lib[dll] += 1
                elif o not in tables[dll]:
                    st, m, rd = "ordinal_not_in_lib", "", ""
                    miss_ord[dll].append(o)
                else:
                    m = tables[dll][o]
                    rd = demangle(m)
                    looks = re.search(r"__(?:[0-9QF]|C\d|t\d)|^_[.$]_", m)
                    st = "ok" if rd != m else ("ok_not_demangled" if looks else "ok_c_symbol")
                    ok += 1
                    nc += st == "ok_c_symbol"
                    undem += st == "ok_not_demangled"
                w.writerow([idx, dll, o, rd, m, st])
    except OSError as e:
        fatal("cannot write %s: %s" % (out, e.strerror or e))
    print("\n%d imports: %d resolved (%d plain C names, %d mangled-looking names NOT demangled), %d unresolved"
          % (len(rows), ok, nc, undem, len(rows) - ok))
    for d, n in sorted(miss_lib.items()):
        print("  no lib for DLL %-16s %3d imports" % (d, n))
    for d, l in sorted(miss_ord.items()):
        print("  ordinals missing in %s.lib (%d of its imports): %s" % (d, len(l), " ".join(map(str, sorted(l)))))
    if miss_ord:
        print("  -> a lib that lacks ordinals the binary imports is from a different SDK/edition")
    return 1 if (a.strict and (len(rows) - ok or undem)) else 0

if __name__ == "__main__":
    sys.exit(main())
