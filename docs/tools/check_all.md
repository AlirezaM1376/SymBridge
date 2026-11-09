# check_all.py and coverage_all.py - the offline gate and the all-games report

## check_all.py - run this before every commit
```
python tools\check_all.py                       all steps, all games
python tools\check_all.py --only manifest,coverage
python tools\check_all.py --game rodland        coverage step for one game only
python tools\check_all.py --verbose             also show the output of passing steps
python tools\check_all.py --cxx C:\mingw\bin\g++.exe
```
(`tools\check_all.cmd` is the same for Windows.) It needs no SDK, no device and asks nothing. Output: a table with one line per
check, the output of every failed or skipped one, and `PASSED: N check(s), F failed, S skipped`. Exit codes: **0** all passed
(a SKIP is not a failure), **1** something failed, **2** bad usage (unknown step, unknown game).

| step | what it proves | tool |
|---|---|---|
| manifest | each `game.json` agrees with its folder and `baseline.log` | `check_manifest.py` |
| importmap | `src\importmap_data.cpp` is exactly what the maps and the curated file generate | `gen_importmap.py --check` |
| direct | every direct bind in `directbind.cpp` is safe and backed by the map | `check_direct.py` |
| tests | all `tools\tests\test_*.py` pass (the tools themselves behave) | `unittest` |
| hosttest | the host's import lookup (`importmap.cpp`) finds what the table holds. Compiled with a **host** g++/clang++; SKIP when none is on `PATH` (the Symbian g++ is never used) | `test_importmap_host.cpp` |
| report | `docs\COVERAGE.md` is up to date | `coverage_all.py --check` |
| coverage | per game: the predicted shim/direct/trap equal the numbers in the manifest. A **pending** game (no baseline yet) is a SKIP | `import_coverage.py` |

When a step fails, its tool's message tells what to run (`gen_importmap.py`, `coverage_all.py`, `accept.py --update`).
Not in the gate: `check_log.py` (needs a device log) and every tool that writes files.

## coverage_all.py - docs\COVERAGE.md
```
python tools\coverage_all.py            write docs\COVERAGE.md
python tools\coverage_all.py --check    exit 1 if the file on disk is not what the data generates (line ends ignored)
python tools\coverage_all.py --out F    write somewhere else
```
Reads every game's `game.json`, main map and `baseline.log`, plus `src\`. Writes a page without dates or machine names, so it
only changes when the data changes. Three parts: **1** one row per game (status, import counts, % handled, reached traps,
frames, run time), **2** the import matrix (every import any game uses, traps first, most-used first, with the games that use
it), **3** static prediction against what the device runs reached ("gap" when a run called a trap). Only each game's main
version counts. Commit the file with the change that made it differ.

Exit codes: **0** done, **1** `--check` found a difference, **2** cannot build (a game folder is unreadable: the message
names the game; no `games` folder; the output cannot be written). The page is written through a temp file.

## Tests
`tools\tests\test_check_all.py` runs both scripts on a scratch copy of the project: a clean copy passes; a missing baseline, a
changed map and a tampered `COVERAGE.md` are each caught; a pending game is skipped; bad usage and unreadable games exit 2.
`test_coverage_all.py` recounts the matrix independently from the map files.
