#!/usr/bin/env python3
"""Generate src/importmap_data.cpp (the S60v2 -> S60v3 import map) from map CSVs.

  python gen_importmap.py                       # games/*/map*.csv + importmap_curated.csv -> src/importmap_data.cpp
  python gen_importmap.py --check               # regenerate in memory, exit 1 if the file on disk differs (the gate)
  python gen_importmap.py --baseline OLD.h      # also report every difference against an old-style importmap.h

Inputs
  games/<id>/map*.csv     output of v2_to_v3.py, one main map (map.csv) and optional other versions (map.alt.csv ...)
                          per game. All of them are merged (union). A new game = add games/<id>/map.csv and run this script.
                          (--maps may also point to one CSV or to a flat folder of CSVs.)
  importmap_curated.csv   hand decisions the SDK libs cannot tell (virtual slots, manual direct binds). Kept apart from the
                          generated columns so regenerating never overwrites them.

How a CSV row becomes a table row (kind = TImportKind in importmap.h)
  status exact, one ctor/dtor variant or plain function      -> 0 EDirect         new = v3_ordinal
  status exact, v3_variant set AND v3_alt non-empty          -> 5 EAmbiguous      new = chosen variant (informational only)
  status name_only                                           -> 6 ESigChanged     new = 0 (signature differs, never bind)
  status moved                                               -> 1 EShimNeeded     new = 0 (moved to another DLL; no direct ordinal recorded)
  status ambiguous (several v3 overloads)                    -> 7 EOverloaded     new = 0 (decide by hand)
  status missing, name starts with "__"                      -> 3 ECompilerHelper new = 0
  status missing, anything else                              -> 1 EShimNeeded     new = 0
  status no_v3_dso                                           -> 4 ENoV3Lib        new = 0
  status no_v2_lib / v2_ord_missing                          -> row skipped (v2 name unknown: same as "not in map")
  Kind 2 (EVirtualSlot) and manual changes come only from importmap_curated.csv.

Merging rules (several CSVs may describe the same (dll, v2 ordinal))
  * the v2 name must be identical, otherwise the files come from different Symbian generations: ERROR
  * a no_v3_dso row loses against any row that has a real answer
  * all other rows must agree on (kind, new), otherwise ERROR (different v3 SDKs, or a generator problem)
  * kind-5 rows may differ in the chosen variant (older maps picked D0): they only have to list the same candidate set;
    the complete-object variant (C1/D1) wins, like in v2_to_v3.py.
All problems are listed at once; exit code 2 means nothing was written.

Other checks: every CSV needs the columns dll, v2_ordinal, v2_name, status (and v3_ordinal for exact rows); an exact row
needs a v3 ordinal of 1..65535; an --overlay named on the command line must exist; an overlay row needs a known kind (name
or 0..7) and a (dll, ordinal) that is in some map, and may appear only once.
--check ignores the line-end style (a checkout with CRLF is still "up to date"). The output file is written through a
temp file, so a failure never leaves a half-written importmap_data.cpp.
Exit codes: 0 done / up to date, 1 --check found the file out of date, 2 errors (nothing written).
"""
import argparse
import csv
import glob
import io
import os
import re
import sys
import tempfile

KIND_NAMES = ["direct", "shim_needed", "virtual_slot", "compiler_helper",
              "no_v3_lib", "ambiguous", "sig_changed", "overloaded"]
KIND_ID = dict((n, i) for i, n in enumerate(KIND_NAMES))
KIND_ENUM = ["EDirect", "EShimNeeded", "EVirtualSlot", "ECompilerHelper",
             "ENoV3Lib", "EAmbiguous", "ESigChanged", "EOverloaded"]
UNRESOLVED = ("no_v2_lib", "v2_ord_missing")
VARIANT_RANK = {"C1": 0, "D1": 0, "C2": 1, "D2": 1, "C3": 2, "C0": 9, "D0": 9}   # same rule as v2_to_v3.py
HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_ROOT = os.path.dirname(HERE)


class Row:
    __slots__ = ("dll", "old", "name", "kind", "new", "status", "src", "cands", "variant")


def norm(s):
    return re.sub(r"\s+", " ", (s or "").strip())


def cand_set(row):
    s = set()
    if norm(row.get("v3_ordinal")):
        s.add(int(row["v3_ordinal"]))
    for part in (row.get("v3_alt") or "").split("|"):
        m = re.match(r"\s*(\d+):", part)
        if m:
            s.add(int(m.group(1)))
    return frozenset(s)


def read_csv(path, need=()):
    """Rows of a CSV (comment lines starting with # are skipped) as dicts. ValueError when a needed column is missing."""
    with io.open(path, newline="", encoding="utf-8-sig") as f:
        lines = [l for l in f if not l.lstrip().startswith("#")]
    rd = csv.DictReader(io.StringIO("".join(lines), newline=""))
    missing = [c for c in need if c not in (rd.fieldnames or [])]
    if missing:
        raise ValueError("%s: missing column(s) %s (found: %s)" % (os.path.basename(path), ", ".join(missing),
                                                                  ", ".join(rd.fieldnames or []) or "none"))
    return list(rd)


def atomic_write(path, text):
    fd, tmp = tempfile.mkstemp(prefix=".tmp_", dir=os.path.dirname(os.path.abspath(path)))
    try:
        with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
        os.replace(tmp, path)
    except BaseException:
        if os.path.exists(tmp):
            os.unlink(tmp)
        raise


def to_row(r, src, errors):
    try:
        dll = norm(r["dll"]).lower()
        old = int(norm(r["v2_ordinal"]))
        status = norm(r["status"])
    except (KeyError, ValueError) as e:
        errors.append("%s: unreadable row %r (%s)" % (src, r, e))
        return None
    if not re.match(r"^[a-z0-9_]+$", dll) or not (0 < old <= 0xFFFF):
        errors.append("%s: bad dll/ordinal in row %r" % (src, r))
        return None
    if status in UNRESOLVED:
        return "skip"
    x = Row()
    x.dll, x.old, x.status, x.src = dll, old, status, os.path.basename(src)
    x.name = norm(r.get("v2_name"))
    x.cands, x.variant, x.new = None, norm(r.get("v3_variant")), 0
    alt = norm(r.get("v3_alt"))
    if status == "exact":
        if not norm(r.get("v3_ordinal")).isdigit():
            errors.append("%s: %s #%d status exact without v3_ordinal" % (src, dll, old))
            return None
        x.new = int(r["v3_ordinal"])
        if not 0 < x.new <= 0xFFFF:
            errors.append("%s: %s #%d v3 ordinal %d is not in 1..65535" % (src, dll, old, x.new))
            return None
        if x.variant and alt:
            x.kind, x.cands = KIND_ID["ambiguous"], cand_set(r)
        else:
            x.kind = KIND_ID["direct"]
    elif status == "name_only":
        x.kind = KIND_ID["sig_changed"]
    elif status == "ambiguous":
        x.kind = KIND_ID["overloaded"]
    elif status == "missing":
        x.kind = KIND_ID["compiler_helper"] if x.name.startswith("__") else KIND_ID["shim_needed"]
    elif status == "moved":
        x.kind = KIND_ID["shim_needed"]
    elif status == "no_v3_dso":
        x.kind = KIND_ID["no_v3_lib"]
    else:
        errors.append("%s: %s #%d unknown status %r" % (src, dll, old, status))
        return None
    if not x.name:
        errors.append("%s: %s #%d has no v2_name" % (src, dll, old))
        return None
    return x


def merge(key, rows, errors):
    where = "%s #%d" % key
    if len(set(r.name for r in rows)) > 1:
        errors.append("%s: v2 name differs between maps (different Symbian generation?): %s" % (
            where, "; ".join("%s: %s" % (r.src, r.name) for r in rows)))
        return None
    real = [r for r in rows if r.status != "no_v3_dso"] or rows
    first = real[0]
    if all(r.kind == KIND_ID["ambiguous"] for r in real):
        if len(set(r.cands for r in real)) > 1:
            errors.append("%s: ctor/dtor candidate sets differ: %s" % (
                where, "; ".join("%s: %s" % (r.src, sorted(r.cands)) for r in real)))
            return None
        return min(real, key=lambda r: VARIANT_RANK.get(r.variant, 5))
    for r in real[1:]:
        if (r.kind, r.new) != (first.kind, first.new):
            errors.append("%s: maps disagree: %s" % (where, "; ".join(
                "%s: %s new=%d" % (r.src, KIND_ENUM[r.kind], r.new) for r in real)))
            return None
    return first


def apply_overlay(table, path, errors, warnings):
    try:
        rows = read_csv(path, ("dll", "v2_ordinal", "kind"))
    except (OSError, ValueError, UnicodeError) as e:
        errors.append("overlay %s: %s" % (path, e))
        return
    seen = set()
    for n, r in enumerate(rows, 2):
        try:
            key = (norm(r["dll"]).lower(), int(norm(r["v2_ordinal"])))
            kind = norm(r["kind"])
            kind = int(kind) if kind.isdigit() else KIND_ID[kind]
            if not 0 <= kind < len(KIND_NAMES):
                raise ValueError("kind number out of range")
        except (KeyError, ValueError, TypeError):
            errors.append("%s row %d: unreadable (%r); kinds: %s" % (os.path.basename(path), n, r, ", ".join(KIND_NAMES)))
            continue
        if key in seen:
            errors.append("%s row %d: %s #%d appears twice" % (os.path.basename(path), n, key[0], key[1]))
            continue
        seen.add(key)
        row = table.get(key)
        if row is None:
            errors.append("%s row %d: %s #%d is in no map CSV (stale overlay row?)" % (os.path.basename(path), n, key[0], key[1]))
            continue
        new = int(norm(r.get("v3_ordinal"))) if norm(r.get("v3_ordinal")).isdigit() else row.new
        if kind == KIND_ID["direct"] and new == 0:
            errors.append("%s row %d: kind direct needs a v3_ordinal" % (os.path.basename(path), n))
            continue
        if (kind, new) == (row.kind, row.new):
            warnings.append("overlay row for %s #%d changes nothing (redundant)" % key)
        row.kind, row.new = kind, new


def render(table, map_files, overlay_file):
    dlls = sorted(set(k[0] for k in table))
    idx = dict((d, i) for i, d in enumerate(dlls))
    keys = sorted(table, key=lambda k: (idx[k[0]], k[1]))
    out = []
    w = out.append
    w("// GENERATED by tools/gen_importmap.py -- DO NOT EDIT BY HAND (edit games/*/map*.csv or tools/importmap_curated.csv).")
    w("// Maps    : %s" % ", ".join(map_files))
    w("// Curated : %s" % (overlay_file or "(none)"))
    w("// Entries : %d in %d DLLs, sorted by (dll index, old ordinal): importmap.cpp binary-searches this table." % (len(keys), len(dlls)))
    w('#include "importmap.h"')
    w("")
    w("const TInt KImportMapDllCount = %d;" % len(dlls))
    w("const char* const KImportMapDllNames[] =")
    w("    {")
    for d in dlls:
        w('    "%s",' % d)
    w("    };")
    w("")
    w("const TInt KImportMapCount = %d;" % len(keys))
    w("const TImportMapEntry KImportMap[] =")
    w("    {")
    for k in keys:
        r = table[k]
        label = r.name.replace("\\", "/")
        w("    { %d, %d, %d, %d },   // %s" % (idx[k[0]], k[1], r.new, r.kind, label))
    w("    };")
    return "\n".join(out) + "\n"


# ---- baseline comparison against the old, hand-carried importmap.h ----------------------------------------------------
def load_old_header(path):
    with io.open(path, encoding="utf-8") as f:
        text = f.read()
    names = dict((int(a), b) for a, b in re.findall(r'case\s+(\d+)\s*:\s*return\s*\(const TText8\*\)"([^"]*)"', text))
    out = {}
    for d, o, n, k, nm in re.findall(r"\{\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\}\s*,?\s*//\s*(.*)", text):
        if int(d) in names:
            out[(names[int(d)], int(o))] = (int(n), int(k), nm.strip())
    return out


def report_baseline(table, path):
    old = load_old_header(path)
    print("baseline %s: %d rows, generated: %d rows" % (os.path.basename(path), len(old), len(table)))
    only_old = sorted(set(old) - set(table))
    only_new = sorted(set(table) - set(old))
    for k in only_old:
        print("  only in baseline : %s #%d  %s" % (k + (old[k][2],)))
    for k in only_new:
        print("  only in generated: %s #%d  %s" % (k + (table[k].name,)))
    diff = 0
    for k in sorted(set(old) & set(table)):
        o, t = old[k], table[k]
        if (o[0], o[1]) != (t.new, t.kind):
            diff += 1
            print("  differs %s #%d %s: baseline new=%d kind=%s, generated new=%d kind=%s" % (
                k + (t.name, o[0], KIND_ENUM[o[1]], t.new, KIND_ENUM[t.kind])))
    print("  %d common rows, %d differ" % (len(set(old) & set(table)), diff))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--root", default=DEFAULT_ROOT, help="project root (default: the parent of tools/)")
    ap.add_argument("--maps", help="games folder, a flat folder of CSVs, or a single CSV (default: <root>/games)")
    ap.add_argument("--overlay", help="default: <root>/tools/importmap_curated.csv")
    ap.add_argument("--out", help="default: <root>/src/importmap_data.cpp")
    ap.add_argument("--check", action="store_true", help="do not write; exit 1 if --out is not up to date")
    ap.add_argument("--baseline", help="old-style importmap.h: list differences against it")
    a = ap.parse_args()
    a.maps = a.maps or os.path.join(a.root, "games")
    explicit_overlay = bool(a.overlay)
    a.overlay = a.overlay or os.path.join(a.root, "tools", "importmap_curated.csv")
    a.out = a.out or os.path.join(a.root, "src", "importmap_data.cpp")

    if os.path.isfile(a.maps):
        files = [a.maps]
    else:
        files = sorted(glob.glob(os.path.join(a.maps, "*", "map*.csv")) + glob.glob(os.path.join(a.maps, "*.csv")))
    if not files:
        print("no map CSVs found in %s" % a.maps, file=sys.stderr)
        return 2
    errors, warnings, groups, skipped = [], [], {}, {}
    for f in files:
        try:
            rows = read_csv(f, ("dll", "v2_ordinal", "v2_name", "status"))
        except (OSError, ValueError, UnicodeError) as e:
            errors.append(str(e))
            continue
        for r in rows:
            x = to_row(r, f, errors)
            if x == "skip":
                skipped[(norm(r["dll"]).lower(), int(norm(r["v2_ordinal"])))] = os.path.basename(f)
            elif x:
                groups.setdefault((x.dll, x.old), []).append(x)
    table = {}
    for key in sorted(groups):
        m = merge(key, groups[key], errors)
        if m:
            table[key] = m
    unresolved = sorted(k for k in skipped if k not in groups)
    if unresolved:
        warnings.append("%d imports skipped, v2 name unknown (no_v2_lib / v2_ord_missing): %s" % (
            len(unresolved), ", ".join("%s #%d" % k for k in unresolved[:12]) + (" ..." if len(unresolved) > 12 else "")))
    overlay_name = None
    if os.path.exists(a.overlay):
        overlay_name = os.path.basename(a.overlay)
        apply_overlay(table, a.overlay, errors, warnings)
    elif explicit_overlay:
        errors.append("--overlay %s does not exist" % a.overlay)
    for w in warnings:
        print("warning: " + w, file=sys.stderr)
    if errors:
        for e in errors:
            print("ERROR: " + e, file=sys.stderr)
        print("%d error(s); nothing written" % len(errors), file=sys.stderr)
        return 2

    labels = [os.path.relpath(f, a.maps).replace("\\", "/") if os.path.isdir(a.maps) else os.path.basename(f) for f in files]
    text = render(table, labels, overlay_name)
    counts = {}
    for r in table.values():
        counts[r.kind] = counts.get(r.kind, 0) + 1
    print("%d entries: %s" % (len(table), ", ".join("%s=%d" % (KIND_ENUM[k], counts[k]) for k in sorted(counts))))
    if a.baseline:
        report_baseline(table, a.baseline)

    out = os.path.normpath(a.out)
    if a.check:
        cur = None
        if os.path.exists(out):
            with io.open(out, newline="", encoding="utf-8") as f:
                cur = f.read().replace("\r\n", "\n")
        if cur != text:
            print("CHECK FAILED: %s is not up to date; run gen_importmap.py" % out, file=sys.stderr)
            return 1
        print("check ok: %s is up to date" % os.path.basename(out))
        return 0
    if not os.path.isdir(os.path.dirname(os.path.abspath(out))):
        print("the folder of %s does not exist" % out, file=sys.stderr)
        return 2
    try:
        atomic_write(out, text)
    except OSError as e:
        print("cannot write %s (%s)" % (out, e.strerror or e), file=sys.stderr)
        return 2
    print("wrote %s" % out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
