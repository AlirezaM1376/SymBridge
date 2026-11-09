# mkbaseline.py, accept.py, mkmanifest.py - from a device log to a permanent test

After the first good run of a game on the phone you have a full device log. These three scripts turn it into the game's
permanent regression test: a small `baseline.log` and the expected numbers in `game.json`. You normally run only
**`accept.py`**; it calls the other two.

```
DEVICE.log --mkbaseline--> games/<id>/baseline.log  --mkmanifest--> games/<id>/game.json (uid3, expect, status)
            \_______________ accept.py = checks + both steps _______________/
```

## accept.py - the one you run
```
python tools\accept.py mygame C:\logs\MyGame_phone.log --status plays
```
It checks the log before writing anything. It **refuses** (exit 1, one `REFUSED:` line per problem) when:
- the log's UID3 is not the game's (wrong log, or wrong game id);
- the run did not end normally (`exit=` in the SUMMARY is not `normal`: crash, hang, killed);
- the numbers predicted from `games\<id>\map.csv` (shim / direct / trap, import count) differ from the log. That means the
  map is not the version that ran, or `gen_importmap.py` was not run before the host was built.

`--force` accepts anyway. Notes (not refusals) are printed when unresolved imports were reached in the run (they appear as
a gap in COVERAGE.md), when the log ran a different file name than `game.json` lists, or when the game's profile file does
not exist.

| option | meaning |
|---|---|
| `id`, `log` | the game folder name under `games\` and the full device log |
| `--status plays\|partial\|menu` | what to record (default `plays`). Use `partial` or `menu` when the game only half works |
| `--update` | replace an existing `baseline.log` (a second run, a newer host build). Without it an existing baseline is never replaced |
| `--force` | accept although a safety check failed (also overwrites a different uid3 in `game.json`) |
| `--dry-run` | do every check, print what would happen, write nothing |
| `--root DIR` | project root (default: the parent of `tools\`) |

A log that contains several runs (several `SymBridge GUI start` lines) is read from its **last** start; a note says so.
If anything fails after `baseline.log` was written, the previous `baseline.log` is put back (or the new one removed), so a
game is never left half accepted.

Exit codes: **0** accepted (or a dry run that would accept), **1** refused by a check, **2** cannot run (no such game, no
map, log missing or not a finished run).

Afterwards: `python tools\coverage_all.py`, then `python tools\check_all.py`.

## mkbaseline.py - the log reducer
```
python tools\mkbaseline.py FULL.log -o games\mygame\baseline.log
```
Keeps only the lines the offline checks use: the start line; `build:`, `OS version`, `client rect`, `locator:`, `profile:`,
`config:`; the `game:` chunk and app lines; `shim registry:`; every `link:`; `hits:`; the total-frames line; `SUMMARY`; the
`thread: game thread` and `exit: game thread` lines; `CRASHWATCH:`. Audio, key, file-system, scheduler and graphics detail
lines are dropped. Output has CRLF line ends. Without `-o` it goes to the screen (as bytes, so CRLF survives on Windows).
Reducing an already reduced log changes nothing (a test checks this for every game).
`--all-runs` keeps every run of a multi-run log instead of the last one only. Exit **2** when the log is missing, has no
`SUMMARY` (the run did not finish), or the `-o` folder does not exist.

## mkmanifest.py - the manifest writer
```
python tools\mkmanifest.py games\mygame\baseline.log [--name "My Game"] [--status plays] [--force]
```
Creates or refreshes `games\<id>\game.json` from the baseline: `uid3`, `app_file` and the main version's `expect` numbers
(imports, direct, shim, trap, reached). Everything you wrote by hand (name, status, notes, profile, code_size, version
notes) is **never** overwritten. Every `map.alt*.csv` in the folder that is not yet listed is added as an `alt` version;
run it again after adding an alternative map. It refuses (exit 1, nothing written) when `game.json` already has another
uid3 than the log: `--force` overwrites it. The file is written through a temp file, so an error leaves the old one intact.
Exit **2** for a missing log, unreadable `game.json` or an unusable log.

## Errors
| message | meaning |
|---|---|
| `accept: no usable games/<id>/game.json` | run `newgame.py` first (or the id is misspelt) |
| `cannot use this log: ... no SUMMARY line` | the run never finished: take the log after the game was closed normally |
| `REFUSED: UID3 in the log is ...` | wrong log for this game |
| `REFUSED: static prediction ... differs from the log` | map and host build do not match; run `gen_importmap.py`, rebuild, run again |
| `baseline.log exists already; use --update` | you are accepting a game twice on purpose: add `--update` |
| `mkmanifest: game.json says uid3 X but the log is from Y` | wrong baseline for this folder |

## Tests
`tools\tests\test_baseline_tools.py` covers multi-run logs, the CRLF output, the atomic write, uid3 mismatch and `--force`,
dry run, the restore after a failed write, and the exit codes. `test_newgame_tools.py` accepts a real baseline into a
scratch project and runs `check_manifest`.
