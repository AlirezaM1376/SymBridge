# gen_importmap.py - build the host's import table from the game maps

The host does not read CSV files at run time. `gen_importmap.py` merges every game's map, applies the hand decisions of
`tools\importmap_curated.csv` and writes **`src\importmap_data.cpp`**, a sorted C++ table the host binary-searches. That
file is generated: never edit it by hand, and commit it together with the maps it came from.

```
games\*\map*.csv  +  tools\importmap_curated.csv  --gen_importmap-->  src\importmap_data.cpp
```

## When to run it
- after `newgame.py` (a new game's map must reach the host before the first device run);
- after you change any `map*.csv` or `importmap_curated.csv`;
- before you build the host. `accept.py` refuses a run whose numbers do not match the map, which is how a forgotten
  regeneration shows up.

## Usage
```
python tools\gen_importmap.py            write src\importmap_data.cpp
python tools\gen_importmap.py --check    write nothing; exit 1 when the file on disk is not what the maps generate
```
`check_all.py` runs the `--check` form. Output: the entry count by kind (`EDirect=263, EShimNeeded=19, ...`) and `wrote ...`
or `check ok`.

| option | meaning |
|---|---|
| `--root DIR` | project root (default: the parent of `tools\`) |
| `--maps PATH` | a games folder (default `<root>\games`), a flat folder of CSVs, or one CSV |
| `--overlay FILE` | the hand decisions (default `<root>\tools\importmap_curated.csv`; it must exist when you name it) |
| `--out FILE` | output (default `<root>\src\importmap_data.cpp`) |
| `--check` | compare only. Line ends do not matter, so a CRLF checkout is still "up to date" |
| `--baseline OLD.h` | list the differences against an old hand-carried `importmap.h` (migration aid) |

Exit codes: **0** done or up to date, **1** `--check` found the file out of date, **2** errors (all listed, nothing written).

## How a map row becomes a table row
The map says what the pipeline found; the table says what the host does with it (`TImportKind`):

| map `status` | kind | `new` ordinal |
|---|---|---|
| `exact`, plain function or one ctor/dtor variant | EDirect | the v3 ordinal |
| `exact` with a chosen variant and a candidate list | EAmbiguous (informational) | the chosen variant |
| `name_only` (name equal, signature changed) | ESigChanged | 0, never bound |
| `ambiguous` (several v3 overloads) | EOverloaded | 0, decide by hand |
| `moved`, or `missing` | EShimNeeded | 0 |
| `missing` and the name starts with `__` | ECompilerHelper | 0 |
| `no_v3_dso` | ENoV3Lib | 0 |
| `no_v2_lib`, `v2_ord_missing` | row skipped (the v2 name is unknown; a warning counts them) | |

EVirtualSlot and hand-made direct binds exist only through the curated file.

## Merging several maps
Every game map is merged (union). For one (dll, v2 ordinal) found in several maps:
- the v2 names must be identical, otherwise the maps come from different Symbian generations: **error**;
- a `no_v3_dso` row loses against any row that has a real answer;
- all other rows must agree on kind and ordinal, otherwise **error** (different v3 SDKs or a generator problem);
- ambiguous ctor/dtor rows may pick different variants but must list the same candidates; the complete-object variant wins.

## The curated file
`tools\importmap_curated.csv` holds what the SDK libraries cannot tell: `dll,v2_ordinal,kind,v3_ordinal,note`. `kind` is a
name (`direct`, `shim_needed`, `virtual_slot`, `compiler_helper`, `no_v3_lib`, `ambiguous`, `sig_changed`, `overloaded`) or a
number 0..7. Rules: the row must match a (dll, ordinal) present in some map (a stale row is an error); `direct` needs a
v3 ordinal (given or from the map); a row may appear once; a row that changes nothing only gives a warning. Lines that start
with `#` are comments.

## Errors
| message | meaning |
|---|---|
| `no map CSVs found in ...` | wrong `--maps` or `--root`, or `games\` is empty |
| `<file>: missing column(s) ...` | not a map made by `v2_to_v3.py` |
| `... status exact without v3_ordinal` / `v3 ordinal ... is not in 1..65535` | a damaged or hand-edited map row |
| `v2 name differs between maps` | two maps for the same DLL ordinal name different functions |
| `maps disagree` | the maps were made against different v3 SDKs; remake the odd one |
| `... is in no map CSV (stale overlay row?)` | the curated row refers to a function no game imports any more |
| `CHECK FAILED: ... is not up to date` | run `python tools\gen_importmap.py` and commit the result |

## Tests
`tools\tests\test_gen_importmap.py` builds synthetic maps and checks every kind, the merge rules and their errors, the
curated file rules, `--check` (missing, current, CRLF, outdated), bad input and write errors, and that the real repository
is up to date.
