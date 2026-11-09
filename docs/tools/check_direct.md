# check_direct.py - is every direct bind safe?

A **direct bind** lets a game's old-ABI import call the v3 function with no wrapper. That only works for plain functions, so
`src\directbind.cpp` keeps a short whitelist (`KDirectBindEntries`) and `check_direct.py` proves, on every run of
`check_all.py`, that no row of it breaks the rules. It never changes any file.

```
src\directbind.cpp (whitelist) + src\importmap_data.cpp (v2 names) --check_direct--> PASS / FAIL lines
```

## Usage
```
python tools\check_direct.py            check the project
python tools\check_direct.py -v         also list every row that passed
python tools\check_direct.py --root D   check another project folder (default: the parent of tools\)
```
Example output:
```
VARIADIC REVIEW: euser #467: TDes16::Format(TRefByValue<const TDesC16>, ...)
PASS: checked 46 direct entries against exact EDirect map rows
```
Exit codes: **0** pass, **1** a row failed, or a table in one of the files cannot be read (a `FAIL:` line says which),
**2** cannot run (a source file is missing; the message goes to stderr).

## What is checked, for every active row
Rows commented out with `//` or `/* */` are ignored. For each active row:
1. it appears **once**;
2. the generated map has a row for this (dll, ordinal) and it is **EDirect** (a name-only, ambiguous or shim-needed map row
   is never bound directly) and carries the v2 function name;
3. the function is safe to bind:
   - no deny-listed name: `qsort`, `bsearch`, `atexit`, `signal`, `setjmp`, `_setjmp`, `longjmp`, `on_exit` (they take
     callbacks or save the stack: rules R2/R3 of the `directbind.cpp` header);
   - no function-pointer parameter (a callback, R2);
   - no `float`, `double`, `TReal`, `TInt64` or `long long` parameter **by value** (the old toolchain passes them with the
     words swapped, R1). By reference or pointer is fine.

**VARIADIC REVIEW** lines are not failures: a function with `...` (R5) is bound only after a human looked at what the games The C library functions `printf`, `fprintf`, `sprintf`, `snprintf`, `sscanf`, `scanf`, `fscanf`, `open`, `ioctl`, `fcntl` get the same line even though their map name has no `...`.
pass it. The line is a reminder to keep that review current.

Not checked by the script, so a human decides when adding a row: that the class layouts the function touches are the same in
both versions (R4), and that the game really needs the bind.

## Adding a direct bind
1. Make sure the game's map is merged (`gen_importmap.py`) and the function shows as EDirect.
2. Add the row to `KDirectBindEntries` with a `// name` comment, as the other rows.
3. Run `python tools\check_direct.py -v`. A FAIL line says which rule the function breaks.
4. Run `check_all.py`.

## Messages
| message | meaning |
|---|---|
| `FAIL: <dll> #<n>: no import-map row` | the whitelist names an import that no map knows: wrong ordinal, or `gen_importmap.py` was not run |
| `... map row is not EDirect: <name>` | the pipeline did not find an exact v3 match: it must stay a shim or a trap |
| `... EDirect map row has no v2 name` | damaged `importmap_data.cpp` (regenerate it) |
| `... : <name>: by-value ABI-sensitive parameter: double` etc. | the function breaks rule R1/R2/R3; remove the row |
| `... listed twice in KDirectBindEntries` | duplicate row |
| `FAIL: could not find KDirectBindEntries table` / `could not parse direct-bind entry` | the table was edited into a shape the script cannot read; keep the one-row-per-line format |
| `check_direct: missing file: ...` | wrong `--root` |

## Tests
`tools\tests\test_check_direct.py`: the real project passes, commented rows are not counted, a row without a map entry or a
duplicate fails, each unsafe parameter kind is refused while references and pointers pass, damaged tables and missing files
give the right exit code, comment markers inside strings survive, and `-v` lists passed rows.
