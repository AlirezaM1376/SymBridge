#!/usr/bin/env python3
"""
Import coverage report for the S60v1/v2 compatibility layer.

  python import_coverage.py --src <project>/src --csv games/<id>/map.csv [--csv ...] [--expect shim=N,direct=N,trap=N]
                            [--out report.md] [--max-list N] [--verbose]

Reads
  * a v2->v3 map CSV for a game (written by v2_to_v3.py; columns
    index,dll,v2_ordinal,v2_name,v3_ordinal,v3_name,status,v3_alt[,v3_variant,note] - the last two are optional)
  * the TShimRegEntry tables in the host source files (<src>/**/*.cpp)
  * the generated <src>/importmap_data.cpp (the merged map used by the host linker) and <src>/iatfill.h (KMaxLinkImports)

and tells, for every import, what the IAT linker would do with it:
  shim   a non-direct host registry entry exists
  direct the registry marks it direct AND the merged map has a usable EDirect row
  trap   everything else: the game gets a trap stub; fine until the game really calls it

--csv        the game's map; give it several times to rank the imports that several games need (only imports that
             really end up as a trap are counted)
--expect     expected linker counts for the FIRST --csv. Without it the numbers come from the expect block of the
             version in games/<id>/game.json next to that CSV; without that nothing is compared and the report says so
--out        write the report to this file instead of the screen
--max-list   most rows per list in the report (default 80)
--verbose    list every registry table read and its entry count

Registry tables are the source of truth. Comments are removed without touching string literals, and "#if 0" blocks
are ignored before the tables are parsed.

The report also
  * warns when the map is incomplete (no_v2_lib / no_v3_dso / v2_ord_missing): those imports show as "trap" only
    because a lib was missing, not because of the game
  * states a BLOCKER when the game has more imports than KMaxLinkImports
  * lists direct-bound imports that are constructors/destructors (v3_variant) or carry a note for a second look
  * cross-checks the map's v3 ordinals against the merged import map
Exit   : 0 done (and, if compared, MATCH), 1 the numbers differ from the expected ones (MISMATCH),
         2 cannot run (a file or table cannot be read, a CSV lacks columns, bad option value; message on stderr).
"""
import argparse
import collections
import csv
import os
import re
import sys


# --------------------------------------------------------------------------- source reading
def read(path):
    with open(path, encoding="utf-8", errors="replace") as fh:
        return fh.read()


def strip_comments(text):
    """Remove /* */ and // comments (newlines are kept, string/char literals are left alone) and '#if 0' blocks."""
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if c in "\"'":
            j = i + 1
            while j < n and text[j] != c and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            out.append(text[i:j + 1])
            i = j + 1
        elif text.startswith("//", i):
            j = text.find("\n", i)
            i = n if j < 0 else j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("\n" * text.count("\n", i, j))
            i = j
        else:
            out.append(c)
            i += 1
    skip, nest, lines = False, 0, []
    for line in "".join(out).split("\n"):
        s = line.strip()
        if skip:
            if re.match(r"#\s*if(n?def)?\b", s):
                nest += 1
            elif re.match(r"#\s*endif\b", s):
                if nest:
                    nest -= 1
                else:
                    skip = False
            elif re.match(r"#\s*else\b", s) and nest == 0:
                skip = False
            lines.append("")
        elif re.match(r"#\s*if\s+0\b", s):
            skip, nest = True, 0
            lines.append("")
        else:
            lines.append(line)
    return "\n".join(lines)


def split_cpp_fields(initializer):
    """Split a C++ aggregate initializer at commas outside nested expressions."""
    fields = []
    depth = 0
    start = 0
    quote = None
    escaped = False
    for index, char in enumerate(initializer):
        if quote:
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == quote:
                quote = None
        elif char in "\"'":
            quote = char
        elif char in "([{":
            depth += 1
        elif char in ")]}":
            depth -= 1
        elif char == "," and depth == 0:
            fields.append(initializer[start:index].strip())
            start = index + 1
    fields.append(initializer[start:].strip())
    return fields


def registry_initializers(body):
    """Yield each top-level aggregate initializer from a registry array body."""
    depth = 0
    start = None
    quote = None
    escaped = False
    for index, char in enumerate(body):
        if quote:
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == quote:
                quote = None
        elif char in "\"'":
            quote = char
        elif char == "{":
            if depth == 0:
                start = index + 1
            depth += 1
        elif char == "}":
            depth -= 1
            if depth < 0:
                raise ValueError("unbalanced braces in shim registry table")
            if depth == 0 and start is not None:
                yield body[start:index]
                start = None
    if depth != 0:
        raise ValueError("unbalanced braces in shim registry table")


def cpp_integer(value):
    value = value.strip().rstrip("uUlL")
    return int(value, 16) if value.lower().startswith("0x") else int(value, 10)


def load_shim_registry(src):
    """Read TShimRegEntry arrays into a (dll, old ordinal) -> policy map."""
    table_re = re.compile(
        r"\b(?:static\s+)?const\s+TShimRegEntry\s+(\w+)\s*\[\s*\]\s*=\s*\{(.*?)\};",
        re.S,
    )
    policies = {}
    tables = []
    for root, dirs, files in os.walk(src):
        dirs.sort()
        for filename in sorted(files):
            if not filename.lower().endswith(".cpp"):
                continue
            path = os.path.join(root, filename)
            relpath = os.path.relpath(path, src)
            text = strip_comments(read(path))
            for table_match in table_re.finditer(text):
                table_name, body = table_match.groups()
                count = 0
                for initializer in registry_initializers(body):
                    fields = split_cpp_fields(initializer)
                    if len(fields) != 6:
                        raise ValueError(
                            f"{relpath}:{table_name}: expected 6 registry fields, got {len(fields)}"
                        )
                    dll_match = re.fullmatch(r'"([^"]+)"', fields[0])
                    if not dll_match or not re.fullmatch(r"[a-z0-9_]+", dll_match.group(1)):
                        raise ValueError(f"{relpath}:{table_name}: invalid DLL name {fields[0]!r}")
                    try:
                        ordinal = cpp_integer(fields[1])
                    except ValueError as exc:
                        raise ValueError(
                            f"{relpath}:{table_name}: invalid ordinal {fields[1]!r}"
                        ) from exc
                    dll = dll_match.group(1)
                    flags = fields[3]
                    if "EShimRegDirect" in flags:
                        policy = "direct"
                    elif "EShimRegResolvedAtRuntime" in flags:
                        policy = "runtime"
                    else:
                        policy = "shim"
                    key = (dll, ordinal)
                    if key in policies:
                        raise ValueError(
                            f"duplicate registry entry: {dll} #{ordinal} "
                            f"({policies[key][1]} and {relpath}:{table_name})"
                        )
                    policies[key] = (policy, f"{relpath}:{table_name}")
                    count += 1
                tables.append((relpath, table_name, count))
    if not tables:
        raise ValueError(f"no TShimRegEntry tables found under {src}")
    return policies, tables


def load_importmap_h(src):
    # Current layout: generated importmap_data.cpp (KImportMapDllNames[] array + KImportMap[] rows).
    # Fallback: the old single-file importmap.h (ImportMapDllName switch + rows).
    p = os.path.join(src, "importmap_data.cpp")
    if os.path.exists(p):
        text = read(p)
        m = re.search(r"KImportMapDllNames\[\]\s*=\s*\{(.*?)\};", text, re.S)
        names = dict(enumerate(re.findall(r'"([^"]*)"', m.group(1)))) if m else {}
    else:
        p = os.path.join(src, "importmap.h")
        if not os.path.exists(p):
            return {}
        text = read(p)
        names = dict((int(a), b) for a, b in re.findall(r'case\s+(\d+)\s*:\s*return\s*\(const TText8\*\)"([^"]*)"', text))
    out = {}
    for d, o, n, k, nm in re.findall(r"\{\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\}\s*,?\s*//\s*(.*)", text):
        if int(d) in names:
            out[(names[int(d)], int(o))] = dict(v3=int(n), kind=int(k), name=nm.strip())
    return out


def load_link_import_limit(src):
    """KMaxLinkImports from <src>/iatfill.h. OSError when the file is missing, ValueError when the constant is not there."""
    header = os.path.join(src, "iatfill.h")
    text = read(header)
    match = re.search(r"^\s*const\s+TInt\s+KMaxLinkImports\s*=\s*(\d+)\s*;", text, re.M)
    if not match:
        raise ValueError("cannot find KMaxLinkImports in %s" % header)
    return int(match.group(1))


MAP_COLUMNS = ("dll", "v2_ordinal", "v2_name", "v3_ordinal", "v3_name", "status")


def load_csv(path, need=("dll", "v2_ordinal")):
    """Rows of a map CSV as dicts. OSError for a missing file; ValueError (with file and row) for a missing column or an
    ordinal that is not a number."""
    with open(path, newline="", encoding="utf-8-sig") as f:
        rd = csv.DictReader(f)
        missing = [c for c in need if c not in (rd.fieldnames or [])]
        if missing:
            raise ValueError("%s: missing column(s) %s (is this a map made by v2_to_v3.py?)" % (path, ", ".join(missing)))
        rows = list(rd)
    for n, r in enumerate(rows, 2):
        if not (r.get("dll") or "").strip() or not (r.get("v2_ordinal") or "").strip().isdigit():
            raise ValueError("%s line %d: bad dll or v2_ordinal (%r, %r)" % (path, n, r.get("dll"), r.get("v2_ordinal")))
        for col in need:
            if r.get(col) is None:
                raise ValueError("%s line %d: too few fields" % (path, n))
    return rows


def classify(dll, old, registry, import_map):
    entry = registry.get((dll, old))
    if entry:
        policy = entry[0]
        if policy in ("shim", "runtime"):
            return "shim"
        mapped = import_map.get((dll, old))
        if policy == "direct" and mapped and mapped["kind"] == 0 and mapped["v3"] != 0:
            return "direct"
    return "trap"


def esc(s):
    return (s or "").replace("|", "\\|")


def expect_from_manifest(csv_path):
    """'shim=N,direct=N,trap=N' from the game.json in the CSV's folder, or None."""
    import json
    folder = os.path.dirname(os.path.abspath(csv_path))
    try:
        with open(os.path.join(folder, "game.json"), "r", encoding="utf-8") as f:
            m = json.load(f)
        for v in m.get("versions", []):
            if v.get("map") == os.path.basename(csv_path) and "expect" in v:
                e = v["expect"]
                return "shim=%d,direct=%d,trap=%d" % (e["shim"], e["direct"], e["trap"])
    except (OSError, ValueError, KeyError):
        pass
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--src", required=True)
    ap.add_argument("--csv", action="append", required=True)
    ap.add_argument("--out")
    ap.add_argument("--expect", help="shim=N,direct=N,trap=N (default: read from game.json next to the first CSV)")
    ap.add_argument("--max-list", type=int, default=80)
    ap.add_argument("--verbose", action="store_true")
    a = ap.parse_args()
    if not os.path.isdir(a.src):
        print("import_coverage: --src %s is not a folder" % a.src, file=sys.stderr)
        return 2

    try:
        registry, tables = load_shim_registry(a.src)
        base = load_importmap_h(a.src)
        max_link_imports = load_link_import_limit(a.src)
        csv_rows = [load_csv(path, MAP_COLUMNS) for path in a.csv]
    except (OSError, ValueError) as exc:
        print(f"import_coverage: cannot read its inputs: {exc}", file=sys.stderr)
        return 2
    if not base:
        print(f"import_coverage: cannot read the import map under {a.src}", file=sys.stderr)
        return 2
    L = []
    P = L.append

    P("# Import coverage report\n")
    shim_count = sum(1 for policy, _ in registry.values() if policy in ("shim", "runtime"))
    direct_count = sum(1 for policy, _ in registry.values() if policy == "direct")
    P(f"Shim registry: **{shim_count}** shim/runtime and **{direct_count}** direct "
      f"(dll, ordinal) entries across {len(tables)} tables.\n")
    if a.verbose:
        P("Shim registry tables read:\n")
        for path, table, count in tables:
            P(f"- `{path}` `{table}`: {count} entr{'y' if count == 1 else 'ies'}")
        P("")

    if base:
        c = collections.Counter(classify(d, o, registry, base) for d, o in base)
        P(f"Merged import map (importmap_data.cpp, {len(base)} imports): shim={c['shim']} direct={c['direct']} trap={c['trap']}\n")

    mismatch = False
    expect_text, expect_from = a.expect, "--expect"
    if not expect_text:
        expect_text = expect_from_manifest(a.csv[0])
        expect_from = "game.json"
    expected_counts = None
    if expect_text:
        expected = re.fullmatch(r"shim=(\d+),\s*direct=(\d+),\s*trap=(\d+)", expect_text.replace(" ", ""))
        if not expected:
            print("import_coverage: invalid --expect; use shim=N,direct=N,trap=N", file=sys.stderr)
            return 2
        expected_counts = tuple(int(value) for value in expected.groups())
    need = collections.defaultdict(set)         # (dll, ordinal) -> games that need it
    trapped = collections.defaultdict(set)      # ... of which games where it ends up as a trap
    label = {}                                  # (dll, ordinal) -> name
    for path, rows in zip(a.csv, csv_rows):
        game = os.path.basename(path)
        P(f"## {game}\n")
        P(f"{len(rows)} imports, {len(set((r['dll'], r['v2_ordinal']) for r in rows))} unique.")
        stat = collections.Counter(r['status'] for r in rows)
        P("Status column: " + ", ".join(f"{k}={v}" for k, v in stat.most_common()) + "\n")
        incomplete = [r for r in rows if r["status"] in ("no_v2_lib", "no_v3_dso", "v2_ord_missing")]
        if incomplete:
            dlls = collections.Counter(r["dll"].lower() for r in incomplete)
            P(f"**WARNING: the map is incomplete.** {len(incomplete)} imports have status no_v2_lib / no_v3_dso / v2_ord_missing "
              f"({', '.join('%s=%d' % kv for kv in dlls.most_common())}). Their names and v3 ordinals are unknown, so unless "
              "a shim covers them they show as trap below only because a lib is missing. Add the missing libs and rebuild the map.\n")
        if len(rows) > max_link_imports:
            P(f"**BLOCKER:** {len(rows)} imports > KMaxLinkImports ({max_link_imports}). `TIatLinker::Link` refuses to link such a game.\n")

        cls = {}
        per_dll = collections.defaultdict(collections.Counter)
        for r in rows:
            d, o = r["dll"].lower(), int(r["v2_ordinal"])
            k = classify(d, o, registry, base)
            cls[(d, o)] = k
            per_dll[d][k] += 1
            need[(d, o)].add(game)
            if k == "trap":
                trapped[(d, o)].add(game)
            if r.get("v2_name"):
                label[(d, o)] = r["v2_name"]
        tot = collections.Counter(cls.values())
        P(f"**Linker outcome: shim={tot['shim']}  direct={tot['direct']}  trap={tot['trap']}**")
        if path == a.csv[0]:
            if expected_counts is None:
                P("Not compared: no --expect given and no game.json next to the CSV.\n")
            else:
                got = (tot["shim"], tot["direct"], tot["trap"])
                mismatch = got != expected_counts
                P(f"Expected ({expect_from}): {expect_text}  ->  **{'MISMATCH' if mismatch else 'MATCH'}**"
                  + (" (the game CSV coverage does not match the baseline)" if mismatch else "") + "\n")
        unknown = sum(1 for r in incomplete if cls[(r['dll'].lower(), int(r['v2_ordinal']))] == "trap")
        if unknown:
            P(f"({unknown} of the {tot['trap']} traps are only traps because no lib was available for them)")
        P("")
        P("| dll | imports | shim | direct | trap |\n|---|---:|---:|---:|---:|")
        for d, cn in sorted(per_dll.items(), key=lambda x: -sum(x[1].values())):
            P(f"| {d} | {sum(cn.values())} | {cn['shim']} | {cn['direct']} | {cn['trap']} |")
        P("")

        if base:
            same = diff = new = 0
            problems = []
            for r in rows:
                key = (r["dll"].lower(), int(r["v2_ordinal"]))
                if key in base:
                    b = base[key]
                    v3 = int(r["v3_ordinal"]) if r["v3_ordinal"].strip() else None
                    if v3 == b["v3"] or (v3 is None and b["v3"] == 0):
                        same += 1
                    else:
                        diff += 1
                        problems.append((key, b["v3"], r["v3_ordinal"], r["v2_name"]))
                else:
                    new += 1
            P(f"Cross-check against the baseline map: {same} imports in common with the same v3 ordinal, "
              f"**{diff} in common with a different v3 ordinal**, {new} new imports.\n")
            for key, bv, nv, name in problems[:40]:
                P(f"- {key[0]} {key[1]} {esc(name)}: baseline v3={bv}, new CSV v3={nv or '(empty)'}")
            if problems:
                P("")

        flagged = [r for r in rows if cls[(r["dll"].lower(), int(r["v2_ordinal"]))] == "direct"
                   and (r.get("v3_variant") or r.get("note"))]
        if flagged:
            P("### Direct-bound, but flagged in the map (second look)\n")
            P("The linker would bind these straight to the v3 ordinal. Constructors/destructors have several v3 variants and the "
              "v2 code cannot call them directly; a note means the match was not a plain signature match.\n")
            P("| dll | v2 | name | variant | note |\n|---|---:|---|---|---|")
            for r in flagged[:a.max_list]:
                P(f"| {r['dll']} | {r['v2_ordinal']} | {esc(r['v2_name'][:70])} | {r.get('v3_variant') or '-'} | {esc((r.get('note') or '-')[:80])} |")
            P("")

        P("### Not exact, and no shim (will be a trap stub)\n")
        P("| dll | v2 | name | status | v3 candidate |\n|---|---:|---|---|---|")
        shown = 0
        for r in rows:
            key = (r["dll"].lower(), int(r["v2_ordinal"]))
            if r["status"] != "exact" and cls[key] == "trap":
                shown += 1
                if shown <= a.max_list:
                    P(f"| {r['dll']} | {r['v2_ordinal']} | {esc(r['v2_name'][:70]) or '-'} | {r['status']} | {esc(r['v3_name'][:40]) or '-'} |")
        if shown > a.max_list:
            P(f"\n... {shown - a.max_list} more (use --max-list)")
        P("")
        P("### Exact in the map but still a trap (needs a shim, or a decision that it is safe to bind directly)\n")
        P("Most of these touch class layouts, virtual slots or callbacks, so only imports explicitly marked direct in the registry and backed by a usable EDirect map row bind directly.\n")
        by = collections.defaultdict(list)
        for r in rows:
            key = (r["dll"].lower(), int(r["v2_ordinal"]))
            if r["status"] == "exact" and cls[key] == "trap":
                by[key[0]].append(r)
        for d, lst in sorted(by.items(), key=lambda x: -len(x[1])):
            P(f"**{d}** ({len(lst)})\n")
            P("| v2 | name | variant |\n|---:|---|---|")
            for r in lst[:a.max_list]:
                P(f"| {r['v2_ordinal']} | {esc(r['v2_name'][:80])} | {r.get('v3_variant') or ''} |")
            if len(lst) > a.max_list:
                P(f"\n... {len(lst) - a.max_list} more (use --max-list)")
            P("")

    if len(a.csv) > 1:
        P("## Imports needed by several games that end up as a trap\n")
        rank = sorted(((len(g), k) for k, g in trapped.items()), key=lambda x: (-x[0], x[1]))
        P("| games | dll | v2 | name |\n|---:|---|---:|---|")
        for n, (d, o) in rank[:60]:
            if n > 1:
                P(f"| {n} | {d} | {o} | {esc(label.get((d, o), '-'))[:80]} |")
    text = "\n".join(L)
    if a.out:
        try:
            with open(a.out, "w", encoding="utf-8") as f:
                f.write(text)
        except OSError as exc:
            print(f"import_coverage: cannot write {a.out} ({exc.strerror or exc})", file=sys.stderr)
            return 2
    else:
        sys.stdout.write(text)
    if mismatch:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
