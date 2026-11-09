# imports_join.py - put names on the imports

A game binary imports system functions by DLL and **ordinal number** only (for example `euser` #81). The names live in the
S60v2 SDK's import libraries (`euser.lib` ...). `imports_join.py` looks every import up in those libraries, turns the old
GCC 2.9x "mangled" names into readable C++ signatures and writes them next to the numbers.

It is step 2 of the map pipeline that `newgame.py` / `triage.py` run:

```
step0.py --dump-imports  ->  skeleton.csv  ->  imports_join.py  ->  joined.csv  ->  v2_to_v3.py  ->  map.csv
```

## Quick start
```
python tools\step0.py GAME.app --dump-imports GAME.skel.csv
python tools\imports_join.py GAME.skel.csv GAME.joined.csv C:\sdk\S60v2_libs
```
Needs only Python and the S60 **v2** `.lib` files (a folder is searched for `*.lib`; you can also name single files). It does
not need the SDK installed, and it does not use the S60 v3 libs (those are for `v2_to_v3.py`).

## Options
| argument | meaning |
|---|---|
| `skeleton` | CSV from `step0.py --dump-imports` (columns `index, dll, old_ordinal`) |
| `out` | the joined CSV to write |
| `LIB_DIR_OR_LIB ...` | S60v2 `.lib` files or folders |
| `--dump-lib-tables DIR` | also write `<dll>.csv` (`ordinal, mangled, readable`) for every lib: handy for looking a name up by hand |
| `--strict` | exit code 1 when an import is unresolved or not demangled |

Exit codes: **0** the file was written (unresolved imports are listed on screen, not an error), **1** `--strict` and something
is unresolved or not demangled, **2** cannot run (reason on stderr).

## Output
`joined.csv` has the columns `index, dll, old_ordinal, readable, mangled, status`.

| status | meaning | what to do |
|---|---|---|
| `ok` | found and demangled, e.g. `TSize::SetSize(int, int)` | nothing |
| `ok_c_symbol` | a plain C name such as `memcpy` | nothing |
| `ok_not_demangled` | found, looks mangled, but the demangler could not parse it; kept as is | the later map step cannot match it by signature; check the name, report it so the demangler can learn it |
| `no_lib` | no lib was given for this DLL | add the lib (the screen lists the DLLs) |
| `ordinal_not_in_lib` | the lib has no such ordinal | the lib is from another SDK or edition than the game was built with; use the right S60v2 lib set |

The screen summary looks like this:
```
lib euser.lib                    dll=euser          1981 ordinals (max 1981)
...
244 imports: 244 resolved (24 plain C names, 0 mangled-looking names NOT demangled), 0 unresolved
```
For a healthy game: 0 not demangled and 0 unresolved. The count of imports must equal the `imports=` number of the host's
`SUMMARY` line.

## How it reads a library
An EKA1 import library is an `ar` archive of small COFF objects. For each object: the ordinal is the `.idata$4` word without
its top bit, the symbol is the object's exported code symbol, and the DLL name is in `.idata$7`. Names built by GCC 2.9x
(the "GNU v2" scheme, for example `SetSize__5TSizeii`) are demangled by the demangler inside the script, because a modern
`c++filt` cannot read that style. A name that cannot be parsed is returned unchanged and never stops the run.

## Errors and messages
| message | meaning |
|---|---|
| `skip X: not an EKA1 (S60v2) import library ...` | you pointed it at S60v3 libs or another kind of file |
| `skip X: no such file or folder` | a wrong path; other libs are still used |
| `no usable .lib file found` (exit 2) | none of the paths gave a readable S60v2 lib |
| `cannot use the skeleton ... column ... missing` (exit 2) | the first argument is not the skeleton from `step0.py --dump-imports` |
| `cannot write ...` (exit 2) | the output folder does not exist or is read-only |
| `note: X already loaded, ... merged into it` | two libs for the same DLL; their ordinals are merged |

## Tests
`tools\tests\test_imports_join.py` builds a small synthetic import library and checks the statuses, `--strict`, the lib
tables, every error exit, the skipping of foreign libs, and the demangler on typical names. It needs no SDK.
