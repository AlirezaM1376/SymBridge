# triage.py - will the host run this game, and what would block it?

`triage.py` takes one old game binary and the SDK libraries, runs the whole static pipeline and writes **one page** that
answers: how many of the game's system calls the host already handles, which ones it does not, and what else in the binary
(code format, heap, screen, files, framework classes) the host needs to cope with. It never touches the host sources.

```
GAME.app --step0--> imports --imports_join--> names --v2_to_v3--> map --prediction + step0 analysis--> triage page
```

`newgame.py` runs it for you; you can also run it alone to look at a game before deciding to add it.

## Quick start
```
python tools\triage.py C:\work\MyGame.app --v2-libs C:\sdk\S60v2_libs --v3-libs C:\sdk\S60v3_libs
```
Writes `work\MyGame.triage.md` and `.txt` (also printed) and the intermediate files in `work\MyGame\`. Needs Python, the SDK
libs and `c++filt` (or `--no-cxxfilt`, which makes the map much weaker).

## Options
| option | meaning |
|---|---|
| `app` | the game binary |
| `--v2-libs`, `--v3-libs` | the SDK lib folders (see `imports_join.md`, `v2_to_v3.md`) |
| `--src DIR` | host sources the prediction is made against (default `<root>\src`) |
| `--out DIR` | output folder (default `<root>\work`) |
| `--strict` | exit code 1 unless the verdict is READY TO TRY |
| `--check-game ID` / `--expect shim=N,direct=N,trap=N` | compare the prediction with known numbers (a game's `game.json`, or by hand) and print MATCH/MISMATCH. Nothing is compared by default |
| `--baseline-app KNOWN.app` | analyse a game the host already runs too and add "new vs baseline" lines (framework classes and slots this game uses that the known one does not) |
| `--sizeof CSV` | `class,size` of the v2 object sizes, to check the allocation sizes of the game's classes |
| `--cxxfilt PATH`, `--no-cxxfilt` | the `c++filt` to use, or none |
| `--max-list N` | most rows per list (default 40) |
| `--root DIR` | project root (default: the parent of `tools\`) |

Exit codes: **0** the report was written, **1** `--strict` and the verdict is not READY TO TRY, **2** a pipeline step failed or
an input is unusable (the message names the step and shows the last lines of its output).

## Files it writes
In `<out>\`: `<game>.triage.md` and `.txt` (the page). In `<out>\<game>\`: `<game>.skel.csv`, `.joined.csv`, `.map.csv` (the
game's map; `newgame.py` copies it to `games\<id>\map.csv`), `.step0.txt` and `.s0.json` (the step-0 analysis),
`<game>.imports.csv` (one row per import with its predicted state and call marks) and `<game>.guess.json` (UID3, the most
likely screen size and display mode, used for the profile stub).

## Reading the page
Start with the first section; the rest is reference.

**Verdict numbers.** Import count against the linker limit, the predicted shim / direct / trap split (the rule is the host's
own: registry says shim → shim; registry says direct and the map has an exact match → direct; everything else → trap), how
many imports the merged import map does not know yet, and three "work predictors":
- unresolved imports (traps) the game **calls directly**,
- traps **reachable from start-up** (static walk from the AppUi's start-up slot),
- traps that only sit in inherited vtable slots (the game never calls them itself: the normal case, for example `HandleCommandL`).

The last line is the **VERDICT**:

| verdict | meaning |
|---|---|
| `BLOCKED` | the host cannot link or decode the game: more imports than the linker limit, or Thumb / mixed ARM-Thumb code |
| `NEEDS WORK` | at least one trap is called or reachable from start-up; see the next section |
| `READY TO TRY` | nothing found that stops a first run. Static analysis only: the device run decides |

For the 8 games tested here the verdicts are READY TO TRY, except Rodland, which is NEEDS WORK because it calls
`RSystemAgent::GetState` (sysagt, no v3 equivalent) directly; its device run never reached it.

**Traps that matter.** The traps from above that are called or reachable, grouped by what to do:

| group | meaning |
|---|---|
| A | plain function the direct-bind rules do not object to: a candidate for `directbind.cpp` after review |
| B | needs a shim in an existing subsystem table (the table is named) |
| C | leave / cleanup stack / timers / active objects: the old trap runtime; never direct |
| D | compiler helpers (integer and float) |
| E | variadic: read the game's format strings first (R5) |
| F | the map row is not exact (ambiguous / name_only / ctor / dtor): decide by hand |
| G | no v3 export at all: a shim or stub is needed |

Marks: `*` reachable from start-up, `+` not in the merged import map yet, `(v)` also in a vtable. If every trap is an
inherited slot the section says "none".

**Imports the host has never seen (+).** Only when the game uses imports that `importmap_data.cpp` lacks. Add the game's map
(`gen_importmap.py`) before judging the numbers.

**DLLs / new subsystems.** The DLLs the game imports from, and those with no registry entry at all (the host has no subsystem
for them yet).

**Compiler helpers, float functions, variadic, threads and timers, direct screen access.** Lists of the imports that need
care, each with its state `[shim]`, `[direct]` or `[trap]` and the marks `C` called, `R` reachable, `V` vtable. They follow
the rules in the header of `directbind.cpp`: R1 = floating-point or 64-bit by value (old toolchain passes doubles with the
words swapped), R2 = callbacks, R3 = setjmp/longjmp, R4 = class layouts that differ, R5 = variadic (printf family). Direct
screen access lists the calls that give the game raw bitmap memory (the host must provide a fake screen).

**Code format.** ARM or Thumb, thunk sizes, build date and tool version, heap and stack. A very large minimum heap is flagged.

**Strings of interest.** Hard-coded absolute paths (a drive letter or `\system\apps`; the host must remap them), data file
names (resolved against the game folder), middleware banners (sound libraries and the like) and the file-system calls worth
a look. Only real paths are listed; lookup tables that happen to contain `:\` no longer count.

**Display mode / resolution.** The display modes the game passes when it creates bitmaps and the likely screen size. It feeds
the profile stub. "not constant" means the mode is chosen at run time.

**Framework classes.** Which framework classes the game derives from (from its vtables) and which expensive kinds it uses
(active objects, timers, dialogs): those are what an old-layout facade has to cover.

**Object sizes before a framework base constructor.** The `new(N)` sizes of the game's own classes (ARM only), compared with
the v2 `sizeof` when `--sizeof` is given.

**Own vtable slots.** Which virtual functions of the framework classes the game overrides, with the predicted input and
foreground route, to compare with the `input route:` line of the device log.

## Errors
| message | meaning |
|---|---|
| `triage: step0 --dump-imports failed ...` etc. | one pipeline step failed; the last lines of its output follow (an unreadable, compressed or Symbian 9 binary shows up here) |
| `c++filt not found` | give `--cxxfilt PATH` or `--no-cxxfilt` |
| `... is not a file`, `--src ... is not a folder` | wrong paths |
| `--check-game ID: cannot read the main expect block` | the game has no accepted baseline yet (pending) |

## Tests
`tools\tests\test_triage.py` runs the whole page on a synthetic game (an image built in the test plus synthetic v2 and v3
libraries): a called trap gives NEEDS WORK and `--strict` exit 1, shimmed imports give READY TO TRY, the output files exist,
and the error exits work. Unit tests cover the path and banner detection and the verdict levels.
