# layout_check.py - does a game match the layout facts of the host?

`docs\LAYOUT_FACTS.md` lists every hard-coded old-layout number. `layout_check.py` checks, for **one game binary**, the facts
that can be read from the binary itself. Run it on a new game before its first device run.

```
python tools\layout_check.py C:\work\MyGame.app --map games\mygame\map.csv
```
`--map` is the game's `map.csv` (names of the imports; `triage.py` and `newgame.py` make it). `--json FILE` keeps step0's analysis,
`--step0 PATH` uses another step0. It runs `step0.py` itself.

## What it checks
| check | fact (LAYOUT_FACTS section) |
|---|---|
| instruction set | ARM only (thunk tests know no Thumb) |
| sizes passed to `CBase::operator new` before `CEikApplication::CEikApplication()`, `CEikAppUi::CEikAppUi()`, `CFbsBitmap::CFbsBitmap()` | 556, 100, 24. The document (36) is also accepted as an unnamed 36-byte allocation when its constructor is called from the game's own code. `CActiveScheduler` is information only |
| words the game overrides in its application / document / AppUi vtables | 14, 19, 15 must be overridden (**DIFF** if not); 19, 8, 9, 3 of the AppUi are reported as INFO (the host asks at run time) |
| control word 3 | `OfferKeyEventL`: INFO when no control overrides it |
| `CActive` / `CIdle` own words | must be inside 2..5 |
| first `ldr rN,[r0,#n]` after each `CCoeEnv::Static()` | `+0x3c` is set by the host and `+0x18` is known (left 0, see LAYOUT_FACTS); **any other offset is a DIFF** because the fake env is zero there |
| stop state | always an INFO line: the profile default is the Captain Dynamo layout |

## Reading the result
`OK` = as documented, `INFO` = nothing to do but worth knowing, `DIFF` = the game differs from the documented layout. A DIFF is
not necessarily a bug (a game may override less); read the matching row of LAYOUT_FACTS before the device run. Exit codes:
**0** no DIFF, **1** at least one DIFF, **2** cannot run (missing file, step0 failed).

## Limits
- It reads instructions with simple patterns (`mov r0,#n` or `ldr r0,[pc,#n]` before the call, `ldr` after `Static()`), not a full
  disassembler. A size set by arithmetic is reported as "not found" (INFO), never as OK.
- The offsets inside a framework object (`iProcess` at +20, `CActive` +4/+20) cannot be seen this way; they stay CODE in LAYOUT_FACTS.
- It never looks at the host sources.
