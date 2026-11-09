# step0.py - first look at a game binary

`step0.py` opens one old game binary (`.app`, S60v1/v2, "EKA1", pre-Symbian-9 E32 format), checks that it is intact and tells
what is inside: which system functions it imports, how it calls them, which classes it derives from and which parts of the
framework its start-up path really uses. It only reads the file. It needs no SDK and no device.

It is the first step of the pipeline that `newgame.py` runs, and you can use it alone to look at a binary.

```
GAME.app --step0 --dump-imports--> skeleton.csv --imports_join--> joined.csv --v2_to_v3--> map.csv
GAME.app --step0 --map joined.csv --json--> analysis.json --triage--> one-page report
```

## Quick start
```
python tools\step0.py GAME.app
```
Prints the report below. Nothing is written. If it stops with `step0: ...` the file cannot be analysed (see "Errors").

| I want to ... | command |
|---|---|
| look at a new binary | `python tools\step0.py GAME.app` |
| start the map pipeline (import list) | `python tools\step0.py GAME.app --dump-imports GAME.skel.csv` |
| see names instead of `?` | add `--map games\<id>\map.csv` (or the joined file of `imports_join.py`) |
| produce the analysis `triage.py` reads | add `--json GAME.json` (normally done by `triage.py` itself) |
| list every thunk, caller, unused import | add `--verbose` |
| use it in a script or CI | add `--strict` and look at the exit code |
| compare with what the phone said | add `--hostlog games\<id>\baseline.log` (and `--expect-traps N`) |

## Options
| option | meaning |
|---|---|
| `app` | the binary |
| `--map CSV` | names for the imports, in import order. Accepts the joined file of `imports_join.py` (columns `dll, old_ordinal, readable`) or a game `map.csv` (columns `dll, v2_ordinal, v2_name`). The first check below verifies that its rows are exactly the binary's imports. |
| `--dump-imports CSV` | write `index,dll,old_ordinal,readable` (readable empty) and carry on |
| `--json FILE` | write the analysis for `triage.py` (keys at the end of this page) |
| `--hostlog LOG` | device log or `baseline.log`: compare its `link: trap` lines with the binary (section 6) |
| `--expect-imports N` | add a check: the binary has exactly N imports (use the number from `game.json`) |
| `--expect-traps N` | with `--hostlog`: add a check: the log reports N traps |
| `--ctor-csv CSV` | compare the reachable import set with an older `constructL_static_imports.csv` |
| `--entry 0x58,0x1234` | start the reachability search at these code offsets (hex) instead of the automatic ones |
| `--mode auto/arm/thumb` | force the instruction set (default: detected from the thunks) |
| `--strict` | exit code 1 when any check failed |
| `--verbose` | list every thunk, called import, unused import and reached import; show one-slot vtables |
| `--cd-checks` | self-test of the analyser on the Captain Dynamo binary only (needs that `.app`); not a coverage check |

Exit codes: **0** the analysis ran (failed checks are listed in the summary, and only change the code with `--strict`),
**1** `--strict` and a check failed, **2** the file cannot be analysed (reason on stderr, no output file written).

## The report, section by section
Example: a real S60v2 game (output shortened).

```
== 1. E32 header and import section
  UIDs 10000079 100039ce 0ba8f6af   compression=0  flags=3
  code=5d1d4 text=5cdfc data=0 bss=0 heap=1000..100000 stack=2000  link base=10000000  DLL refs=13
  type: DLL (entry point not called)   UID2: application (.app)
  [PASS] UID checksum is valid  -> file 90ab2522, computed 90ab2522
  [PASS] section offsets/sizes lie inside the file  -> ...
    euser                     81 imports
  total imports: 244
  [PASS] IAT found and its words equal the import ordinals  -> IAT at code offset 0x5cdfc (244 words)
  instruction set: ARM (0 Thumb-style and 244 ARM-style import thunks)
== 2. Import thunks
  [PASS] one thunk per import  -> 244 thunks for 244 imports
== 3. Vtables
  vtable @5cc94 this-adj 0  slots 13  inherited 10 (+1 via stub)  base~CEikApplication own: [5]=19458 [14]=19468
== 4. Direct callers of imports
  imports called directly: 138   only in vtables: 106   address taken (indirect call): 0   not found: 0
== 5. Static reachability
  roots: AppUi slot 15 (1)
  49 own functions, 37 distinct imports reached (of 244)
== Summary
  7 checks, 7 passed, 0 failed
```

**1. Header and imports.** The third UID is the game's UID3 (the one the host and `game.json` use). `code`/`text` are the
sizes of the code section and of the part that is real code (the import address table follows it). The import list shows how
many functions are imported from each system DLL; the total must equal the `imports=` of the host's `SUMMARY` line. The
checks make sure the file is what it claims to be: UID checksum, offsets inside the file, the import table parses, the IAT
(the table the loader fills) holds exactly the import ordinals, the relocation table is consistent. A failed check usually
means a damaged file, a new format or an analysis that should not be trusted.

**2. Thunks.** Every import is called through a small stub (a "thunk"). It must find exactly one per import. ARM (16 or 12
bytes) and Thumb stubs are recognised from the IAT slot they load, so the layout does not have to be known in advance. If
this fails the later sections say so instead of printing zeros.

**3. Vtables.** Tables of function pointers found with the help of the relocation table. For each: `slots` = number of
entries, `inherited` = entries that point straight at an import (the game does not override them), `via stub` = inherited
through a small forwarding stub, `base~` = the framework class most inherited entries belong to (needs `--map`), `own` =
`[slot]=code offset` of the functions the game overrides. Slot 2 is the destructor. This is what tells which framework
classes the game derives from (`CEikApplication`, `CEikAppUi`, `CCoeControl` ...) and which virtual functions it implements;
the host needs this for input and foreground handling. The line `export #1 stores vtable ...` names the application class.

**4. Callers.** For every import: called directly from the game's own code, only present in a vtable (inherited, never
called by the game itself), address taken (called through a pointer), or not found. `only in vtables` imports are the ones
that never run unless the framework calls them.

**5. Reachability.** Starting at slot 15 of the AppUi class (if the class is found by name) or, otherwise, at all
exports and all own vtable slots, follows `bl` and `b` jumps and collects the imports it meets. It does not follow calls
through vtables, function pointers or switch tables, so it is a lower bound of what start-up uses, not a complete list.

**6. Host log** (only with `--hostlog`). Reads the `link: trap` lines of a device log and says which of the traps the game
calls directly. Those would be hit if the code were executed. Checked on 8 of the 10 games (the other 2 binaries were not at hand): 7 have none, Rodland has one, which its device run never reached.

**Summary.** Number of checks and which failed.

## Errors
| message (stderr) | meaning |
|---|---|
| `... is not an E32 image` | not an old Symbian binary, or the wrong file (for example a map CSV) |
| `header format N ... Symbian 9+ image` | a newer (J/V header) binary; this script only reads the pre-9 format |
| `COMPRESSED image` | the code is compressed (Deflate/BytePair); decompress it first |
| `sections lie outside the file` | damaged or truncated file |
| `cannot use --map ...` | the CSV lacks the columns above, or is not readable |
| `--entry needs hex code offsets` | for example `--entry 0x58,0x1234` |
| `[FAIL] import section equals the map CSV` | the map belongs to a different binary or version |
| `[FAIL] one thunk per import` / `[WARN] no import thunks` | the stubs have an unknown shape; see section 2 |

## Limits
- Heuristic analysis of ARM and Thumb code; not a disassembler. Mixed ARM/Thumb binaries are decoded in one mode only (a
  warning says so).
- Reachability follows direct branches only (see section 5).
- Only uncompressed pre-Symbian-9 images.

## The JSON file (`--json`)
Read by `triage.py`; keys: `app, uid3, cpu, priority, tools, built, heapmin, heapmax, stack, mode, mixed, code_file_offset,
code_size, text_size, code_base, thunk_sizes, imports[{index,dll,ordinal,name}], thunks{offset: import index}, called,
vtable_only, address_taken, not_found, callers{import index: [call-site offsets]}, reach, reach_roots, roots, exports,
vtables[{start,this_adj,base,nslots,own,own_legacy,slot_imports,inherited,via_stub,imports}], checks[[name, ok]]`.
Numbers are decimal; offsets are code-relative.

## Tests
`tools\tests\test_step0.py` builds a tiny synthetic image (one function, two thunks, a relocation table) and checks the
analysis, both `--map` formats, `--strict`, `--expect-*`, `--hostlog` and every error exit. It needs no game.
