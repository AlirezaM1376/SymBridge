#!/usr/bin/env python3
"""Check direct bindings against the v2 import names and the generated import map.

  python check_direct.py [--root DIR] [-v]      (default root: the parent of tools/)

Reads  : <root>/src/directbind.cpp (table KDirectBindEntries) and <root>/src/importmap_data.cpp
Checks : every ACTIVE direct-bind row (commented-out rows are ignored)
           * is listed once only,
           * has an EDirect import-map row with a v2 name,
           * names a function that is safe to bind: no deny-listed function (qsort, bsearch, atexit, signal, the
             setjmp family), no function-pointer parameter, no float/double/64-bit value parameter (rules R1-R3 of
             the header of directbind.cpp; class layouts R4 and format strings R5 are a human review).
Output : FAIL lines, a VARIADIC REVIEW line for each variadic function, and a PASS line with the row count.
         -v also lists every row that passed.
Exit   : 0 pass, 1 a row failed or a table cannot be read (FAIL line), 2 cannot run (a source file is missing: message on stderr).
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
DENIED_NAMES = {
    "qsort",
    "bsearch",
    "atexit",
    "signal",
    "setjmp",
    "longjmp",
    "_setjmp",
    "on_exit",
}
# C library functions that take "..." but whose map name has no "..." in it (the C++ ones carry it in the signature).
C_VARIADIC_NAMES = {
    "printf", "fprintf", "sprintf", "snprintf", "vsprintf", "sscanf", "scanf", "fscanf",
    "open", "ioctl", "fcntl", "syslog", "execl", "execlp",
}
UNSAFE_VALUE_TYPES = re.compile(
    r"\b(?:double|float|TReal|TInt64|long\s+long)\b"
)


def read_source(path: Path) -> str:
    if not path.is_file():
        raise FileNotFoundError(f"missing file: {path}")
    return path.read_text(encoding="utf-8")


def parse_map(source: str) -> dict[tuple[str, int], tuple[int, str]]:
    dll_block = re.search(
        r"KImportMapDllNames\s*\[\s*\]\s*=\s*\{(.*?)\};", source, re.S
    )
    rows_block = re.search(r"KImportMap\s*\[\s*\]\s*=\s*\{(.*?)\};", source, re.S)
    if not dll_block or not rows_block:
        raise ValueError("could not find generated import-map tables")

    dlls = re.findall(r'"([^"]+)"', dll_block.group(1))
    rows: dict[tuple[str, int], tuple[int, str]] = {}
    row_pattern = re.compile(
        r"\{\s*(\d+)\s*,\s*(\d+)\s*,\s*\d+\s*,\s*(\d+)\s*\}"
        r"\s*,?\s*(?://\s*(.*))?"
    )
    for line in rows_block.group(1).splitlines():
        if not line.strip().startswith("{"):
            continue
        match = row_pattern.fullmatch(line.strip())
        if not match:
            raise ValueError(f"could not parse generated import-map row: {line.strip()}")
        dll_index, ordinal, kind = (int(match.group(i)) for i in range(1, 4))
        if dll_index >= len(dlls):
            raise ValueError(f"import-map DLL index {dll_index} is out of range")
        key = (dlls[dll_index].lower(), ordinal)
        if key in rows:
            raise ValueError(f"duplicate import-map row: {key[0]} #{key[1]}")
        rows[key] = (kind, (match.group(4) or "").strip())
    return rows


def strip_comments(source: str) -> str:
    """Remove // and /* */ comments; string and character literals are left alone."""
    out, i, n = [], 0, len(source)
    while i < n:
        c = source[i]
        two = source[i:i + 2]
        if two == "//":
            j = source.find("\n", i)
            i = n if j < 0 else j
        elif two == "/*":
            j = source.find("*/", i + 2)
            i = n if j < 0 else j + 2
            out.append(" ")
        elif c in "\"'":
            j = i + 1
            while j < n and source[j] != c:
                j += 2 if source[j] == "\\" else 1
            out.append(source[i:j + 1])
            i = j + 1
        else:
            out.append(c)
            i += 1
    return "".join(out)


def parse_direct_entries(source: str) -> list[tuple[str, int]]:
    source = strip_comments(source)
    table = re.search(
        r"KDirectBindEntries\s*\[\s*\]\s*=\s*\{(.*?)\};", source, re.S
    )
    if not table:
        raise ValueError("could not find KDirectBindEntries table")

    entries = []
    for initializer in re.findall(r"\{([^{}]*)\}", table.group(1)):
        if "EShimRegDirect" not in initializer:
            raise ValueError(f"non-direct entry in KDirectBindEntries: {{{initializer}}}")
        match = re.match(
            r'\s*"([^"]+)"\s*,\s*(\d+)\s*,\s*[^,]+,\s*EShimRegDirect\b',
            initializer,
        )
        if not match:
            raise ValueError(f"could not parse direct-bind entry: {{{initializer}}}")
        entries.append((match.group(1).lower(), int(match.group(2))))
    if not entries:
        raise ValueError("KDirectBindEntries is empty")
    return entries


def parameter_list(name: str) -> list[str]:
    """Return top-level parameters from a demangled function name."""
    open_paren = name.find("(")
    if open_paren < 0:
        return []

    depth = 0
    angle_depth = 0
    bracket_depth = 0
    params = []
    start = open_paren + 1
    for index in range(open_paren + 1, len(name)):
        char = name[index]
        if char == "<":
            angle_depth += 1
        elif char == ">" and angle_depth:
            angle_depth -= 1
        elif char == "[":
            bracket_depth += 1
        elif char == "]" and bracket_depth:
            bracket_depth -= 1
        elif char == "(":
            depth += 1
        elif char == ")" and depth:
            depth -= 1
        elif char == ")" and angle_depth == 0 and bracket_depth == 0:
            tail = name[index + 1 :].strip()
            if tail and not re.fullmatch(r"(?:const|volatile|noexcept|&|&&|\s)+", tail):
                continue
            last = name[start:index].strip()
            if last:
                params.extend(part.strip() for part in split_parameters(last))
            return [] if params == ["void"] else params
        elif char == "," and depth == 0 and angle_depth == 0 and bracket_depth == 0:
            params.append(name[start:index].strip())
            start = index + 1
    return []


def split_parameters(parameters: str) -> list[str]:
    pieces = []
    depth = angle_depth = bracket_depth = 0
    start = 0
    for index, char in enumerate(parameters):
        if char == "(":
            depth += 1
        elif char == ")" and depth:
            depth -= 1
        elif char == "<":
            angle_depth += 1
        elif char == ">" and angle_depth:
            angle_depth -= 1
        elif char == "[":
            bracket_depth += 1
        elif char == "]" and bracket_depth:
            bracket_depth -= 1
        elif char == "," and depth == angle_depth == bracket_depth == 0:
            pieces.append(parameters[start:index].strip())
            start = index + 1
    pieces.append(parameters[start:].strip())
    return pieces


def unsafe_reasons(name: str) -> list[str]:
    reasons = []
    if re.search(r"\(\s*\*", name):
        reasons.append("function-pointer parameter")

    for parameter in parameter_list(name):
        if parameter == "...":
            continue
        if ("&" not in parameter and "*" not in parameter
                and UNSAFE_VALUE_TYPES.search(parameter)):
            reasons.append(f"by-value ABI-sensitive parameter: {parameter}")

    function_prefix = name.split("(", 1)[0].rstrip()
    function_match = re.search(r"([A-Za-z_]\w*)$", function_prefix)
    if function_match and function_match.group(1) in DENIED_NAMES:
        reasons.append(f"deny-listed name: {function_match.group(1)}")
    return reasons


def check(entries, map_rows, verbose=False):
    """Check the direct rows against the map. Returns (errors, variadic review lines, passed lines)."""
    errors, variadics, passed = [], [], []
    seen = set()
    for dll, ordinal in entries:
        key = (dll, ordinal)
        if key in seen:
            errors.append(f"{dll} #{ordinal}: listed twice in KDirectBindEntries")
            continue
        seen.add(key)
        row = map_rows.get(key)
        if row is None:
            errors.append(f"{dll} #{ordinal}: no import-map row")
            continue
        kind, name = row
        if kind != 0:  # EDirect
            errors.append(f"{dll} #{ordinal}: map row is not EDirect: {name}")
            continue
        if not name:
            errors.append(f"{dll} #{ordinal}: EDirect map row has no v2 name")
            continue
        if "..." in name or re.match(r"^(\w+)\b", name).group(1) in C_VARIADIC_NAMES:
            variadics.append(f"VARIADIC REVIEW: {dll} #{ordinal}: {name}")
        reasons = unsafe_reasons(name)
        for reason in reasons:
            errors.append(f"{dll} #{ordinal}: {name}: {reason}")
        if not reasons:
            passed.append(f"ok   {dll} #{ordinal}: {name}")
    return errors, variadics, passed


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--root", default=str(ROOT), help="project root (default: the parent of tools/)")
    ap.add_argument("-v", "--verbose", action="store_true", help="also list the rows that passed")
    a = ap.parse_args()
    root = Path(a.root).resolve()
    try:
        map_text = read_source(root / "src" / "importmap_data.cpp")
        direct_text = read_source(root / "src" / "directbind.cpp")
    except OSError as exc:
        print(f"check_direct: {exc}", file=sys.stderr)
        return 2
    try:
        map_rows = parse_map(map_text)
        entries = parse_direct_entries(direct_text)
    except ValueError as exc:       # the files are there but a table is damaged: that is a failed check
        print(f"FAIL: {exc}")
        return 1

    errors, variadics, passed = check(entries, map_rows)
    if a.verbose:
        for line in passed:
            print(line)
    for line in variadics:
        print(line)
    if errors:
        for error in errors:
            print(f"FAIL: {error}")
        return 1

    print(f"PASS: checked {len(entries)} direct entries against exact EDirect map rows")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
