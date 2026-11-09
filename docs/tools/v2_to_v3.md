# v2_to_v3.py - map old imports to the S60v3 functions

A game built for S60v2 calls system functions by DLL and ordinal. On an S60v3 phone the same function has another ordinal
(or no longer exists). `v2_to_v3.py` pairs every import of the game with the S60v3 function of the same name and writes the
result as **`map.csv`**, the file that `games\<id>\` keeps and `gen_importmap.py` turns into the host's import table.

Step 3 of the map pipeline:
```
step0.py --dump-imports -> skeleton.csv -> imports_join.py -> (names) -> v2_to_v3.py -> map.csv
```
(`v2_to_v3.py` reads the v2 libs itself, so it takes the skeleton from step 1; the joined file of step 2 works as well.)

## Quick start
```
python tools\v2_to_v3.py --v2 C:\sdk\S60v2_libs --v3 C:\sdk\S60v3_libs --imports GAME.skel.csv -o map.csv
```
Needs Python, the S60v2 `.lib` files, the S60v3 libraries and `c++filt` (GNU binutils; modern builds are fine: the v3 names
are in the modern Itanium style). `.dso` files (from `epoc32\release\armv5\lib`) are preferred; if a DLL has only a `.lib`
the script uses that. The `.lib` route lacks the vtable/typeinfo exports (`_ZTV`, `_ZTI`) but gave the same maps for all
games tested.

## Options
| option | meaning |
|---|---|
| `--v2 LIB_OR_DIR ...` | S60v2 EKA1 `.lib` files or folders |
| `--v3 DSO_OR_DIR ...` | S60v3 `.dso` (or `.lib`) files or folders |
| `--imports CSV` | the game's import list (`index, dll, old_ordinal`). Without it the map lists every ordinal of every v2 lib that has a v3 match (the "full SDK map" mode) |
| `-o, --out CSV` | the map to write |
| `--cxxfilt PATH` | the `c++filt` to use (default: from PATH) |
| `--no-cxxfilt` | work without it: no row can be `exact`, results are much weaker |
| `--require-complete` | exit code 1 when a row is `no_v2_lib` or `v2_ord_missing` |

Exit codes: **0** written, **1** `--require-complete` and a v2 lib is missing or from another SDK, **2** cannot run (reason
on stderr: unreadable `--imports`, no lib read, unwritable output). If `c++filt` is not found the script warns loudly and
continues.

## Output: `map.csv`
Columns: `index, dll, v2_ordinal, v2_name, v3_ordinal, v3_name, status, v3_alt, v3_variant, note`.

| status | meaning | what the host does with it |
|---|---|---|
| `exact` | same full signature found in v3 | may be bound directly (if the host's registry allows it) |
| `name_only` | same `Class::name`, but the signature differs or is unchecked | never bound directly |
| `ambiguous` | several v3 overloads, no safe choice (see `v3_alt`) | decided by hand |
| `missing` | no such function in v3 (removed or renamed) | shim needed; names starting with `__` are compiler helpers |
| `moved` | `memmove`/`memset` moved from `estlib` to `euser` | shim |
| `no_v3_dso` | no v3 library for that DLL (for example `plpvariant`, which S60v3 does not have) | shim or trap |
| `no_v2_lib` | no v2 library for that DLL: the name is unknown | trap unless shimmed |
| `v2_ord_missing` | the v2 lib has no such ordinal (wrong SDK edition) | trap unless shimmed |

`note` explains rows that were not a plain match; **read the rows that have one**:

| note | meaning |
|---|---|
| `const changed` | v2 and v3 differ only in const-ness of the method (`name_only`) |
| `ARITY-GUESS` | several overloads in v3; the only one with the same number of parameters and at least half identical types was taken. A guess: check the differing types by hand. One-parameter overloads are never guessed |
| `alias` | equal after treating `TInt64` as `long long` and `wchar_t` as unsigned. `TInt64` passed by value is `name_only` (class vs built-in 64-bit type) |
| `placeholder` | v3 only keeps a `_placeholder_<name>` stub: the export was removed |
| `overload removed` | the only v3 symbol with that name is the exact match of another v2 overload |
| `share the single` | several v2 overloads would map to one v3 symbol; the pairing cannot be told |

Constructors and destructors: v3 exports several variants (`C1/C2`, `D0/D1/D2`). The chosen one is in `v3_variant`, the others
in `v3_alt`. The rule is fixed (complete-object first, deleting destructor never preferred) so that every game's map picks the
same one. A row that is `exact` **and** has a non-empty `v3_alt` is a multi-variant ctor/dtor: not a safe direct bind.

## Reading the screen summary
After `wrote N rows`: ctor/dtor variants chosen, status counts per DLL, the rows with notes (guesses and by-value `TInt64`
are listed in full), then the lists of `ambiguous`, `missing`, `moved` and `v2_ord_missing` rows (first 40 each) and the DLLs
without a v3 or v2 library. `INCOMPLETE MAP` means imports have no v2 name; the `Note:` line about DLLs without a v3 library
is normal for a DLL that does not exist in S60v3.

## Checking the pipeline against the repository
With the SDK libs and a game binary you can reproduce a committed map exactly:
```
python tools\step0.py GAME.app --dump-imports GAME.skel.csv
python tools\v2_to_v3.py --v2 S60v2_libs --v3 S60v3_libs --imports GAME.skel.csv -o GAME.map.csv
fc /b GAME.map.csv games\<id>\map.csv          (Linux: cmp)
```
For the 8 games checked (Another World, Atomanic, Candy, Captain Dynamo, MagicPuz, Nebulus, Qwak, Rodland) the result is
byte-identical to `games\<id>\map.csv`.

## Errors and messages
| message | meaning |
|---|---|
| `skip v2/v3 X: no symbols read (... swapped?)` | a lib of the wrong generation: `--v2` and `--v3` are probably swapped |
| `no usable S60v2 lib read` / `no usable S60v3 .dso/.lib read` (exit 2) | nothing could be read from that argument |
| `WARNING: c++filt not found` | the map will have no `exact` rows; give `--cxxfilt PATH` |
| `WARNING: c++filt failed on the v3 names of X` | that DLL's rows are `name_only`/`ambiguous` only |
| `note: v3 X taken from .lib (no .dso)` | normal when only `.lib` files are given |
| `WARNING: ... only the deleting destructor/odd variant exists in v3` | check that row by hand |
| `cannot use --imports ...` (exit 2) | not a skeleton/joined CSV |

## Tests
`tools\tests\test_v2_to_v3.py` builds a synthetic v2 library and a synthetic v3 `.dso` and checks the statuses (with and
without `c++filt`; the `c++filt` test is skipped when it is not installed), `--require-complete`, DLLs without a v3 library,
swapped arguments and the error exits. The matching rules themselves are covered by the byte-identical reproduction above.
