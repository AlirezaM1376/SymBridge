# import_coverage.py - what will the host do with each import?

For one game's map, `import_coverage.py` works out exactly what the host's import linker (`TIatLinker`) would do with every
import, without a device. The answer for each import is one of:

| outcome | meaning |
|---|---|
| **shim** | the host has its own implementation (a non-direct entry in a `TShimRegEntry` table, or one resolved at run time) |
| **direct** | the registry marks it direct **and** the merged import map has a usable EDirect row with a v3 ordinal |
| **trap** | everything else: the game gets a stub that does nothing until the game really calls it |

The counts must equal what the device log says (`link: direct=.. shim=.. trap=..`); `accept.py` and `check_all.py` rely on
that. It is also the engine that `triage.py`, `accept.py` and `coverage_all.py` import their rule from.

## Usage
```
python tools\import_coverage.py --src src --csv games\mygame\map.csv
python tools\import_coverage.py --src src --csv games\a\map.csv --csv games\b\map.csv --out work\both.md
```
| option | meaning |
|---|---|
| `--src DIR` | the host sources (`src\`): registry tables in `*.cpp`, `importmap_data.cpp`, `iatfill.h` (the import limit) |
| `--csv FILE` | a game map. Give it several times to rank the imports several games need (only traps are counted) |
| `--expect shim=N,direct=N,trap=N` | numbers to compare with, for the **first** `--csv`. Default: the expect block of the matching version in the `game.json` next to the CSV. With neither, nothing is compared and the report says so |
| `--out FILE` | write the report there instead of the screen |
| `--max-list N` | most rows per list (default 80) |
| `--verbose` | list each registry table read and its entry count |

Exit codes: **0** done (and MATCH if compared), **1** MISMATCH (the numbers differ from the expected ones), **2** cannot run
(missing folder or file, damaged registry table, a CSV without the expected columns or with a non-numeric ordinal, bad
`--expect`; the message says which).

## The report
1. **Header**: how many registry entries were read, and the shim / direct / trap split of the whole merged import map.
2. Per game: import count, the `status` column counts, then
   - **WARNING: the map is incomplete** when imports have status `no_v2_lib`, `no_v3_dso` or `v2_ord_missing`. Those show as
     traps only because a lib was missing; add the libs and rebuild the map before you trust the numbers.
   - **BLOCKER** when the game has more imports than `KMaxLinkImports`.
   - **Linker outcome** with the MATCH / MISMATCH line, and a per-DLL table.
   - **Cross-check against the merged map**: imports in common with the same v3 ordinal, with a different one (a
     disagreement between this game's map and what the host has), and new ones.
   - **Direct-bound, but flagged**: direct imports that are constructors/destructors or carry a note, for a second look.
   - **Not exact, and no shim**: traps whose map row is not an exact match.
   - **Exact in the map but still a trap**: functions that exist in v3 but no shim or direct bind covers; a candidate list for
     new shims.
3. With several CSVs: **imports needed by several games that end up as a trap**, most wanted first.

## How the registry is read
Every `*.cpp` under `--src` is scanned for `const TShimRegEntry NAME[] = { ... };`. Comments are removed without touching
strings, and `#if 0` blocks are ignored. A row must have 6 fields, a quoted lower-case DLL name and a numeric ordinal; a
(dll, ordinal) pair may appear only once in the whole registry. Any violation is reported with the file and table name.

## Tests
`tools\tests\test_import_coverage.py`: comment and `#if 0` stripping, field splitting, the shim / direct / trap rule on every
case, registry errors (duplicate, short row), and the report on a tiny synthetic project: MATCH / MISMATCH, expect from
`game.json`, BLOCKER, incomplete-map warning, several-game ranking, and every error exit.
