#!/usr/bin/env python3
"""v2_to_v3.py - map S60v2 (EKA1) import ordinals to S60v3 (EKA2) ones by symbol name. Writes the game's map.csv.

  python v2_to_v3.py --v2 V2_LIBS --v3 V3_LIBS --imports skeleton.csv -o map.csv [--cxxfilt PATH | --no-cxxfilt]
                     [--require-complete]

Step 3 of the map pipeline (step0.py -> imports_join.py -> v2_to_v3.py). For every import of a game: take its v2 name from
the v2 lib, look for the same function in the v3 lib of that DLL, and record the v3 ordinal and how sure the match is.

Reads  : --v2  S60v2 .lib files or folders (EKA1; imports_join.py must be next to this file)
         --v3  S60v3 .dso files or folders (epoc32/release/armv5/lib). Ordinal = stub slot: dynsym value/4 + 1.
               A .lib is used only for a DLL that has no .dso (same ordinals, but no _ZTV/_ZTI vtable entries)
         --imports  skeleton or joined CSV from step0.py --dump-imports (columns index, dll, old_ordinal): one row per
               import, in script order. Without it: one row per exported v2 ordinal of every v2 lib that has a v3 match.
         c++filt (GNU binutils) to demangle the v3 names; without it only name_only/ambiguous matching is possible.
Writes : -o map.csv and a summary on the screen.
Exit   : 0 = written; 1 = --require-complete and a v2 lib is missing or lacks an ordinal the game imports (a row is
         no_v2_lib / v2_ord_missing: wrong or incomplete SDK); 2 = cannot run (unreadable input, no lib read, cannot write).
         A DLL with no v3 counterpart (no_v3_dso, for example plpvariant, which S60v3 does not have) is reported but is
         not an error.
Output columns: index,dll,v2_ordinal,v2_name,v3_ordinal,v3_name,status,v3_alt,v3_variant,note
status: exact (full signature) | name_only (class::name unique, signature differs/unchecked) |
    ambiguous (several v3 overloads, see v3_alt) | missing (no such name in v3) | moved (memmove/memset moved from estlib to euser) |
    no_v3_dso | no_v2_lib | v2_ord_missing
note:   explains how a row was matched when it was not a plain signature match. Rows with a note should be read:
        "const changed"   v2 and v3 differ only in const-ness of the method (status name_only)
        "ARITY-GUESS"     several v3 overloads; the only one with the same number of parameters (>= 2) and at least
                          half of the parameter types identical was taken (status name_only). This is a GUESS,
                          check the differing parameter types by hand. One-parameter overloads are never guessed.
        "alias"           equal after treating TInt64 as long long and 'unsigned wchar_t' as wchar_t (v3 typedefs).
                          TInt64 passed BY VALUE is name_only, not exact (class vs built-in 64-bit type)
        "placeholder"     v3 only keeps a _placeholder_<name> stub for it: the export was removed (status missing)
        "overload removed" the only v3 symbol with that name is the exact match of another v2 symbol, so this v2
                          overload has no counterpart (status missing)
        "share the single" several v2 overloads would map to one v3 symbol; which is which cannot be told (ambiguous)
The v3 side is demangled with c++filt (Itanium ABI) when available; without it only name_only/ambiguous are possible.

Constructor/destructor variants: v3 exports several per class (Itanium ABI): C1/D1 = complete object, C2/D2 = base object,
D0 = deleting destructor (calls operator delete). The old (GCC 2.95) ABI cannot call any of them directly, so the choice is
informational, but it must be the SAME in every map. Rule (VARIANT_RANK): complete-object first, base-object second, the
deleting destructor only if nothing else exists. The chosen one is in v3_variant, the others stay in v3_alt, so a row with
status=exact AND a non-empty v3_alt is a multi-variant ctor/dtor: never treat it as a safe direct bind.
(Older maps picked the lowest ordinal, which for destructors is D0 = the deleting one.)
"""
import argparse, csv, glob, os, re, shutil, struct, subprocess, sys, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from imports_join import lib_table, demangle, ar_members

# lower = preferred. Unknown / no variant -> 5 (so plain overloads keep the old "lowest ordinal first" order).
VARIANT_RANK = {"C1": 0, "D1": 0, "C2": 1, "D2": 1, "C3": 2, "C0": 9, "D0": 9}

# ---------------------------------------------------------------- v3 .dso (ELF32 ARM)
def dso_table(path):
    with open(path, "rb") as fh:
        b = fh.read()
    if b[:4] != b"\x7fELF":
        raise ValueError("not ELF")
    shoff, = struct.unpack_from("<I", b, 0x20)
    shentsize, shnum = struct.unpack_from("<HH", b, 0x2E)
    secs = [struct.unpack_from("<10I", b, shoff + i * shentsize) for i in range(shnum)]
    tab, dll, odd = {}, None, 0
    for s in secs:
        if s[1] != 11:                                   # SHT_DYNSYM
            continue
        stro = secs[s[6]][4]
        for i in range(s[5] // 16):
            nm, val, _, _, _, shn = struct.unpack_from("<IIIBBH", b, s[4] + 16 * i)
            if not shn:
                continue
            raw = b[stro + nm:b.index(b"\0", stro + nm)].decode("latin1")
            name, _, ver = raw.partition("@@")
            if val % 4:
                odd += 1
                continue
            tab[val // 4 + 1] = name
            if dll is None and ver:
                dll = re.sub(r"[\[{].*$", "", ver).lower()
    if odd:
        print("warning: %s: %d symbols with unaligned stub address skipped" % (path, odd))
    return dll or os.path.basename(path).split("{")[0].split(".")[0].lower(), tab

def v3_lib_table(path):
    """Fallback when no .dso exists: EKA2 import .lib = ar archive of ELF members named <dll>{ver}-<ordinal>.o.
    Same ordinals/names as the .dso but without the vtable/typeinfo exports (_ZTV/_ZTI), which live in VtblExports.o."""
    ms = ar_members(path)
    lng = next((b for n, b in ms if n == "//"), b"")
    tab, dll = {}, None
    for n, b in ms:
        if n in ("/", "//") or b[:4] != b"\x7fELF":
            continue
        if n.startswith("/") and n[1:].isdigit():
            o = int(n[1:])
            n = lng[o:lng.index(b"/\n", o)].decode("latin1")
        mo = re.match(r"^(.*?)\{[0-9a-fA-F]+\}-(\d+)\.o/?$", n.replace("\\", "/").split("/")[-1])
        if not mo:
            continue
        dll = dll or mo.group(1).lower()
        shoff, = struct.unpack_from("<I", b, 0x20)
        es, sn = struct.unpack_from("<HH", b, 0x2E)
        secs = [struct.unpack_from("<10I", b, shoff + i * es) for i in range(sn)]
        for s in secs:
            if s[1] != 2:
                continue
            stro = secs[s[6]][4]
            for i in range(s[5] // 16):
                nm, _, _, _, _, shn = struct.unpack_from("<IIIBBH", b, s[4] + 16 * i)
                e = b[stro + nm:b.index(b"\0", stro + nm)].decode("latin1")
                if shn and e and e not in ("StubCode", ".directive", "theImportedSymbol") and e[0] not in "$.":
                    tab[int(mo.group(2))] = e
    return dll or os.path.basename(path).split("{")[0].split(".")[0].lower(), tab

# ---------------------------------------------------------------- name helpers
OP3 = {"aS": "=", "eq": "==", "ne": "!=", "pl": "+", "mi": "-", "ml": "*", "dv": "/", "rm": "%", "cl": "()",
       "ix": "[]", "ls": "<<", "rs": ">>", "lt": "<", "gt": ">", "le": "<=", "ge": ">=", "pL": "+=",
       "mI": "-=", "nw": " new", "dl": " delete", "na": " new[]", "da": " delete[]", "aa": "&&", "oo": "||",
       "nt": "!", "pp": "++", "mm": "--", "ad": "&", "or": "|", "eo": "^", "co": "~", "pt": "->", "cm": ","}

def v3_loose(m):
    """(qualifier, name, const, variant) from an Itanium name; None for templates/substitutions/data we don't parse."""
    mo = re.match(r"_ZTV(\d+)", m)
    if mo:
        n = int(mo.group(1))
        return ("", "vtable for " + m[mo.end():mo.end() + n], False, None)
    mo = re.match(r"_ZN(K?)", m)
    if mo:
        p, comps, var, const = mo.end(), [], None, bool(mo.group(1))
        while p < len(m) and m[p] != "E":
            if m[p].isdigit():
                q = p
                while m[q].isdigit():
                    q += 1
                n = int(m[p:q])
                comps.append(m[q:q + n])
                p = q + n
            elif m[p] in "CD" and p + 1 < len(m) and m[p + 1] in "0123":
                var = m[p:p + 2]
                comps.append("~" if m[p] == "D" else "")
                p += 2
            elif m[p:p + 2] in OP3:
                comps.append("operator" + OP3[m[p:p + 2]])
                p += 2
            else:
                return None
        if p >= len(m) or not comps:
            return None
        name = comps[-1]
        if name in ("", "~") :
            name += comps[-2] if len(comps) > 1 else ""
        return ("::".join(comps[:-1]), name, const, var)
    mo = re.match(r"_Z(\d+)", m)
    if mo:
        n = int(mo.group(1))
        return ("", m[mo.end():mo.end() + n], False, None)
    return None

def split_top(head):
    depth = 0
    for i in range(len(head) - 1, 0, -1):
        c = head[i]
        depth += (c == ">") - (c == "<")
        if depth == 0 and head[i - 1:i + 1] == "::":
            return head[:i - 1], head[i + 1:]
    return "", head

def v2_loose(readable):
    """(qualifier, name, const, variant) from our v2 demangler output."""
    mo = re.match(r"^vtable for (.*)$", readable)
    if mo:
        return ("", readable, False)
    mo = re.match(r"^(.*?)\((.*)\)( const)?$", readable)
    if not mo:
        return None
    q, n = split_top(mo.group(1))
    return (q, n, bool(mo.group(3)))

def canon(s):
    """normalise c++filt / v2 demangler output so equal signatures compare equal"""
    if s is None:
        return None
    const_m = s.endswith(") const")
    if const_m:
        s = s[:-6]
    for _ in range(4):
        s = re.sub(r"((?:(?:unsigned|signed|long) )*[\w:]+(?:<[^<>]*>)?) const(?![\w])", r"const \1", s)
    s = re.sub(r"\s+", " ", s).replace(" ,", ",").replace("( ", "(").replace(" )", ")")
    return s + (" const" if const_m else "")

def alias(s):
    """canon() + the v3 typedefs: TInt64 is long long, wchar_t is unsigned in both versions"""
    if s is None:
        return None
    s = canon(s).replace("unsigned wchar_t", "wchar_t")
    s = re.sub(r"\bTInt64\b", "long long", s)
    s = re.sub(r"\bTUint64\b", "unsigned long long", s)
    return canon(s)

def params(sig):
    """list of parameter strings of a demangled signature, None if it cannot be told"""
    if sig.endswith(" const"):
        sig = sig[:-6]
    if not sig.endswith(")"):
        return None
    depth, i = 0, len(sig) - 1
    while i >= 0:
        depth += (sig[i] == ")") - (sig[i] == "(")
        if depth == 0:
            break
        i -= 1
    if i < 0:
        return None
    body, depth, out, cur = sig[i + 1:-1], 0, [], ""
    for c in body:
        depth += c in "<(["
        depth -= c in ">)]"
        if c == "," and depth == 0:
            out.append(cur.strip())
            cur = ""
        else:
            cur += c
    if body.strip() in ("", "void"):
        return []
    return out + [cur.strip()]

def run_cxxfilt(names, exe):
    try:
        out = subprocess.run([exe], input="\n".join(names) + "\n", capture_output=True, text=True, timeout=120)
        lines = out.stdout.splitlines()
        return lines if len(lines) == len(names) else None
    except Exception:
        return None

# ---------------------------------------------------------------- main
def collect(srcs, ext):
    out = []
    for s in srcs:
        out += sorted(glob.glob(os.path.join(s, "*" + ext))) if os.path.isdir(s) else [s]
    return out

def fatal(msg):
    print("v2_to_v3: " + msg, file=sys.stderr)
    sys.exit(2)


def main():
    ap = argparse.ArgumentParser(prog="v2_to_v3.py", description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--v2", nargs="+", required=True, metavar="LIB_OR_DIR", help="S60v2 (EKA1) .lib files or folders")
    ap.add_argument("--v3", nargs="+", required=True, metavar="DSO_OR_DIR", help="S60v3 .dso (or, as a fallback, .lib) files or folders")
    ap.add_argument("--imports", metavar="CSV", help="skeleton/joined CSV of the game's imports (default: every v2 ordinal)")
    ap.add_argument("-o", "--out", required=True, metavar="CSV", help="the map to write")
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--cxxfilt", default="c++filt", help="c++filt executable (default: from PATH)")
    g.add_argument("--no-cxxfilt", action="store_true", help="work without c++filt (many more rows end up name_only/ambiguous)")
    ap.add_argument("--require-complete", action="store_true",
                    help="exit 1 when a row is no_v2_lib / v2_ord_missing (a v2 lib is missing or from another SDK)")
    a = ap.parse_args()

    v2 = {}
    for p in collect(a.v2, ".lib"):
        try:
            d, t = lib_table(p)
            if not t:
                print("skip v2 %s: no symbols read (is this an S60v2 EKA1 import library? --v2 and --v3 swapped?)" % p)
                continue
            v2.setdefault(d, {}).update(t)
        except Exception as e:
            print("skip v2 %s: %s" % (p, e))
    v3raw, v3src = {}, {}
    for p in collect(a.v3, ".dso"):
        try:
            d, t = dso_table(p)
            v3raw.setdefault(d, {}).update(t)
            v3src[d] = ".dso"
        except Exception as e:
            print("skip v3 %s: %s" % (p, e))
    for p in collect(a.v3, ".lib"):                      # only for DLLs that have no .dso
        try:
            d, t = v3_lib_table(p)
            if d in v3raw:
                continue
            if not t:
                print("skip v3 %s: no symbols read (is this an S60v3 import library? --v2 and --v3 swapped?)" % p)
                continue
            v3raw[d] = t
            v3src[d] = ".lib"
            print("note: v3 %s taken from .lib (no .dso): vtable/typeinfo exports not available" % d)
        except Exception as e:
            print("skip v3 %s: %s" % (p, e))
    if not v2:
        fatal("no usable S60v2 lib read from: %s" % " ".join(a.v2))
    if not v3raw:
        fatal("no usable S60v3 .dso/.lib read from: %s" % " ".join(a.v3))
    print("v2 libs: %d   v3 DLLs: %d (%d from .dso, %d from .lib)" % (
        len(v2), len(v3raw), sum(1 for x in v3src.values() if x == ".dso"), sum(1 for x in v3src.values() if x == ".lib")))

    exe = None if a.no_cxxfilt else shutil.which(a.cxxfilt)
    if exe is None and not a.no_cxxfilt:
        print("WARNING: %s not found: continuing WITHOUT c++filt, so no row can be 'exact'. Give its path with "
              "--cxxfilt, or use --no-cxxfilt to silence this." % a.cxxfilt)
    v3 = {}
    for d, t in v3raw.items():
        ords = sorted(t)
        full = run_cxxfilt([t[o] for o in ords], exe) if exe else None
        if exe and full is None:
            print("WARNING: c++filt failed on the v3 names of %s: its rows can only be name_only/ambiguous" % d)
        info = {}
        for k, o in enumerate(ords):
            lo = v3_loose(t[o])
            vm = re.search(r"([CD][0-3])E", t[o])
            info[o] = {"m": t[o], "full": canon(full[k]) if full else None, "al": alias(full[k]) if full else None,
                       "lo": lo[:3] if lo else None,
                       "var": lo[3] if lo else (vm.group(1) if vm else None), "show": full[k] if full else t[o]}
        v3[d] = {"info": info, "byfull": collections.defaultdict(list), "bylo": collections.defaultdict(list),
                 "byal": collections.defaultdict(list)}
        for o, x in info.items():
            if x["full"]:
                v3[d]["byfull"][x["full"]].append(o)
                v3[d]["byal"][x["al"]].append(o)
            if x["lo"]:
                v3[d]["bylo"][x["lo"]].append(o)
    print("c++filt: %s" % (exe or "not used (only name_only/ambiguous matching)"))

    def collapse(d, ords):
        """ctor C1/C2, dtor D0/D1/D2 variants -> one preferred ordinal + the rest"""
        info = v3[d]["info"]
        groups = collections.OrderedDict()
        for o in sorted(ords):        # same name up to the C1/C2/D0/D1/D2 variant digit = one group; overloads stay apart
            groups.setdefault(re.sub(r"([A-Za-z0-9_])([CD])[0-3]E", r"\1\2xE", info[o]["m"]), []).append(o)
        best = []
        for g in groups.values():
            best.append(min(g, key=lambda o: (VARIANT_RANK.get(info[o]["var"], 5), o)))
        return best

    def alts(d, cand, chosen, key="m"):
        T = v3[d]["info"]
        return " | ".join("%d:%s" % (o, T[o][key]) for o in cand if o != chosen)

    def match0(d, name):
        """-> (status, v3 ordinal or None, v3_alt, note, candidate list if ambiguous else None); never guesses"""
        if d not in v3:
            return "no_v3_dso", None, "", "", None
        rd = demangle(name)
        T = v3[d]
        have_full = bool(T["info"]) and any(x["full"] for x in T["info"].values())
        cand = T["byfull"].get(canon(rd), []) if have_full else []
        if cand:
            c = collapse(d, cand)
            if len(c) == 1:
                return "exact", c[0], alts(d, cand, c[0]), "", None
        if rd == name:                                    # plain C symbol (memset, __divsi3 ...): same name or nothing
            c = [o for o, x in T["info"].items() if x["m"] == name]
            if c:
                return "exact", c[0], "", "", None
            ph = [o for o, x in T["info"].items() if x["m"] == "_placeholder_" + name]
            return "missing", None, "", ("placeholder: v3 keeps only a stub (ordinal %d), the export was removed" % ph[0]
                                         if ph else ""), None
        # same signature once TInt64 / wchar_t are treated as the v3 typedefs
        cand = T["byal"].get(alias(rd), []) if have_full else []
        if cand:
            c = collapse(d, cand)
            if len(c) == 1:
                byval = re.search(r"\bTInt64\b(?![&*\w])", rd) is not None
                return ("name_only" if byval else "exact"), c[0], alts(d, cand, c[0]), (
                    "alias: TInt64 passed by value, class vs long long, check the ABI" if byval else
                    "alias: TInt64/wchar_t typedef difference only"), None
        lo = v2_loose(rd)
        note = ""
        cand = T["bylo"].get(lo, []) if lo else []
        if not cand and lo and not lo[0].startswith("vtable"):     # API became const / non-const in v3
            cand = T["bylo"].get((lo[0], lo[1], not lo[2]), [])
            if cand:
                note = "const changed: v2 %s, v3 %s" % ("const" if lo[2] else "non-const",
                                                         "non-const" if lo[2] else "const")
        if not cand:
            return "missing", None, "", "", None
        c = collapse(d, cand)
        if len(c) == 1:
            return "name_only", c[0], alts(d, cand, c[0]), note, None
        return "ambiguous", None, " | ".join("%d:%s" % (o, T["info"][o]["show"]) for o in c), note, c

    cache0, guesses, prepared = {}, {}, set()

    def prepare(d):
        """first pass over EVERY v2 symbol of a DLL: remember the plain matches, then pair up the leftover overloads"""
        if d in prepared or d not in v3 or d not in v2:
            return
        prepared.add(d)
        T = v3[d]["info"]
        for o2, nm in v2[d].items():
            cache0[(d, o2)] = match0(d, nm)
        # one v3 symbol can only be the counterpart of one v2 symbol. If a name_only match points at a v3 symbol that
        # another v2 symbol matches exactly, that v2 overload has no counterpart (removed in v3); if several name_only
        # matches share one v3 symbol, the pairing cannot be told.
        by_t = collections.defaultdict(list)
        for (dd, o2), r in cache0.items():
            if dd == d and r[1] is not None:
                by_t[r[1]].append(o2)
        for t3, os_ in by_t.items():
            if len(os_) < 2:
                continue
            ex = [o2 for o2 in os_ if cache0[(d, o2)][0] == "exact"]
            loose = [o2 for o2 in os_ if cache0[(d, o2)][0] == "name_only"]
            if len(ex) == 1:
                for o2 in loose:
                    cache0[(d, o2)] = ("missing", None, "", "v2 overload removed in v3: the only v3 symbol with this name "
                                       "(#%d %s) is the exact match of v2 #%d" % (t3, T[t3]["show"], ex[0]), None)
            elif not ex and len(loose) > 1:
                for o2 in loose:
                    cache0[(d, o2)] = ("ambiguous", None, "%d:%s" % (t3, T[t3]["show"]),
                                       "%d v2 overloads (%s) share the single v3 symbol #%d, pairing cannot be told" % (
                                           len(loose), ", ".join("#%d" % x for x in loose), t3), [t3])
        claimed = {r[1] for (dd, _), r in cache0.items() if dd == d and r[1] is not None}
        fams = collections.defaultdict(list)                  # same candidate set = same overload family
        for (dd, o2), r in cache0.items():
            if dd == d and r[0] == "ambiguous":
                fams[tuple(sorted(r[4]))].append(o2)
        taken = set()
        for cands, items in fams.items():
            left = [o for o in cands if o not in claimed]
            for k in sorted({len(params(demangle(v2[d][o2])) or []) for o2 in items}):
                if k < 2:
                    continue                                   # one-parameter overloads are never guessed
                i2 = [o2 for o2 in items if len(params(demangle(v2[d][o2])) or []) == k]
                i3 = [o for o in left if params(T[o]["full"] or "") is not None and len(params(T[o]["full"])) == k
                      and o not in taken]
                if len(i2) != 1 or len(i3) != 1:
                    continue
                p2, pv = params(demangle(v2[d][i2[0]])), params(T[i3[0]]["full"])
                eq = sum(1 for x, y in zip(p2, pv) if alias(x) == alias(y))
                if eq >= 1 and 2 * eq >= k:
                    taken.add(i3[0])
                    guesses[(d, i2[0])] = (i3[0], " | ".join("%d:%s" % (o, T[o]["show"]) for o in cands if o != i3[0]),
                                           "ARITY-GUESS: %d overloads in v3; the only unclaimed one with %d parameters, "
                                           "%d of %d types identical, the rest differ: check by hand" % (len(cands), k, eq, k))

    def match(d, o2):
        """-> (status, v3 ordinal or None, v3_alt, note)"""
        prepare(d)
        r = cache0.get((d, o2)) or match0(d, v2[d][o2])
        if (d, o2) in guesses:
            g = guesses[(d, o2)]
            return "name_only", g[0], g[1], (r[3] + "; " if r[3] else "") + g[2]
        return r[:4]

    rows = []
    if a.imports:
        try:
            with open(a.imports, encoding="utf-8-sig", newline="") as fh:
                for r in csv.DictReader(fh):
                    rows.append((r["index"], r["dll"], int(r["old_ordinal"])))
        except (OSError, KeyError, ValueError) as ex:
            fatal("cannot use --imports %s: %s (columns index, dll, old_ordinal are needed)" % (a.imports, ex))
        if not rows:
            fatal("--imports %s has no rows" % a.imports)
    else:
        for d in sorted(v2):
            if d in v3:
                rows += [("", d, o) for o in sorted(v2[d])]
    stat = collections.defaultdict(collections.Counter)
    miss = collections.defaultdict(list)
    variants = collections.Counter()
    noted = []
    try:
        fh = open(a.out, "w", encoding="utf-8", newline="")
    except OSError as ex:
        fatal("cannot write %s: %s" % (a.out, ex.strerror or ex))
    with fh:
        w = csv.writer(fh)
        w.writerow(["index", "dll", "v2_ordinal", "v2_name", "v3_ordinal", "v3_name", "status", "v3_alt", "v3_variant", "note"])
        for idx, d, o in rows:
            if d not in v2:
                st, v2n, o3, alt, nt = "no_v2_lib", "", None, "", ""
            elif o not in v2[d]:
                st, v2n, o3, alt, nt = "v2_ord_missing", "", None, "", ""
            else:
                v2n = demangle(v2[d][o])
                st, o3, alt, nt = match(d, o)
                if d == "estlib" and v2n.lower() in ("memmove", "memset") and st == "missing":
                    st = "moved"
                    nt = "moved to euser in S60v3; no v3 ordinal is recorded for the estlib import"
            v3n = v3[d]["info"][o3]["show"] if o3 else ""
            v3var = (v3[d]["info"][o3]["var"] or "") if o3 else ""
            if v3var:
                variants[v3var] += 1
                if v3var == "D0" or v3var == "C0":
                    print("WARNING: %s #%d: only the deleting destructor/odd variant exists in v3, check by hand" % (d, o))
            w.writerow([idx, d, o, v2n, o3 if o3 else "", v3n, st, alt, v3var, nt])
            if nt:
                noted.append((st, d, o, v2n, nt))
            stat[d][st] += 1
            if st not in ("exact", "name_only"):
                miss[st].append((d, o, v2n))
    print("\nwrote %d rows to %s" % (len(rows), a.out))
    if variants:
        print("ctor/dtor variants chosen: " + "  ".join("%s=%d" % kv for kv in sorted(variants.items())) +
              "   (rule: complete-object first, deleting destructor never preferred)\n")
    else:
        print()
    for d in sorted(stat):
        print("  %-16s %s" % (d, "  ".join("%s=%d" % kv for kv in sorted(stat[d].items()))))
    if noted:
        print("rows with a note (%d), read these:" % len(noted))
        kinds = collections.Counter(n.split(":")[0].split(";")[0] for _, _, _, _, n in noted)
        print("  " + "  ".join("%s=%d" % kv for kv in sorted(kinds.items())))
        for st, d, o, n_, nt in noted:
            if "ARITY-GUESS" in nt or "by value" in nt:
                print("  [%s] %s #%d %s\n        %s" % (st, d, o, n_, nt))
        print()
    for st in ("ambiguous", "missing", "moved", "v2_ord_missing"):
        if miss[st]:
            print("\n%s (%d):" % (st, len(miss[st])))
            for d, o, n in miss[st][:40]:
                print("  %s #%d %s" % (d, o, n))
            if len(miss[st]) > 40:
                print("  ... %d more in the CSV" % (len(miss[st]) - 40))
    for st, label in (("no_v3_dso", "DLLs with no v3 .dso"), ("no_v2_lib", "DLLs with no v2 lib")):
        ds = sorted(set(d for d, _, _ in miss[st]))
        if ds:
            print("\n%s: %s" % (label, ", ".join(ds)))
    no_v2 = sum(len(miss[k]) for k in ("no_v2_lib", "v2_ord_missing"))
    no_v3 = len(miss["no_v3_dso"])
    if no_v2:
        print("\nINCOMPLETE MAP: %d import(s) have no v2 name (a v2 lib is missing or from another SDK edition); their "
              "names and v3 ordinals are unknown." % no_v2)
    if no_v3:
        print("\nNote: %d import(s) are in a DLL with no v3 lib/dso. That is fine when the DLL does not exist in S60v3 "
              "(for example plpvariant); otherwise add the lib. Such imports are shim or trap in the host." % no_v3)
    return 1 if (no_v2 and a.require_complete) else 0

if __name__ == "__main__":
    sys.exit(main())
