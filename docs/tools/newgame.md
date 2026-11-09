# newgame.py - bring a new game into the repository

`newgame.py` is the first command for a game the host has never seen. It runs `triage.py` (the whole static analysis, see
[triage.md](triage.md)) and then creates the game's folder and a starting profile. It does not change the host sources and
it never overwrites a game that has an accepted run.

```
GAME.app + SDK libs --triage--> report + map --newgame--> games/<id>/{map.csv,game.json}  +  profiles/<UID3>.prf
```

## Quick start
```
python tools\newgame.py C:\work\MyGame.app --v2-libs C:\sdk\v2\lib --v3-libs C:\sdk\v3\lib --name "My Game"
```
You get the triage page on screen, then a summary:
```
created games/mygame/map.csv and game.json (status not_tested, pending: no baseline yet)
created profiles/0BA8F6AF.prf
triage page: ...\work\MyGame.triage.md
VERDICT: READY TO TRY: ...
NEXT STEPS ...
```
Read the page first. If the VERDICT is BLOCKED, the folder is still created (the map is useful), but the game will not run
until the cause is fixed.

## Options
| option | meaning |
|---|---|
| `app` | the game binary (never commit it) |
| `--v2-libs`, `--v3-libs` | the SDK lib folders |
| `--id ID` | folder name under `games\`. Default: the file name in lower case, letters and digits only (`My Game.app` gives `mygame`) |
| `--name TEXT` | display name for `game.json` and the profile header (default: the file name) |
| `--cxxfilt PATH`, `--no-cxxfilt` | as in triage.py |
| `--work DIR` | where triage writes its files (default `<root>\work`) |
| `--root DIR` | project root (default: the parent of `tools\`) |
| `--dry-run` | run triage, print what would be created, write nothing in `games\` or `profiles\` |
| `--force` | replace `games\<id>` when it exists **and is still pending** |

## What it creates
- `games\<id>\map.csv`: the game's import map, from the pipeline. Merged into the host by `gen_importmap.py`.
- `games\<id>\game.json`: `id`, `name`, `uid3`, `app_file`, `status: not_tested`, `profile`, and one `main` version without
  baseline or expect numbers. Such a game is **pending**: `check_manifest.py` and `check_all.py` accept it and skip its
  coverage check. `accept.py` turns it into a normal game after the first good device run.
- `profiles\<UID3>.prf`: screen size and display mode as far as triage could tell them, every other key commented out. Only
  keys the host's profile parser knows are written (a test checks that). Check each value on a device, then delete the
  "guessed" header line. An existing profile is never touched.

After writing, the script runs the same check as `check_manifest.py` on the new folder, so a broken stub is reported at once.

## Safety rules
| situation | result |
|---|---|
| `games\<id>` exists | refused (exit 2). `--force` replaces it only while it has no `baseline.log` and no baseline/expect in `game.json` |
| `games\<id>` has an accepted run | refused even with `--force`: remove or rename it by hand if you really want to start again |
| the game's UID3 belongs to another game | refused (exit 2): a second build of the same game is an extra version, see below |
| writing fails half way | a folder created by this run is removed again |
| `--dry-run` | nothing written in `games\` or `profiles\` (triage's `work\` files are written) |

## Another build of a game you already have
Same UID3 means the same game, so it is a version of it, not a new game. Run triage alone on the new build
(`python tools\triage.py NEW.app ...`), copy `work\<name>\<name>.map.csv` to `games\<id>\map.alt.csv`, then run
`python tools\mkmanifest.py games\<id>\baseline.log` to list it in `game.json`, and add a note saying what differs
(see "Another version of the same game" in BRINGING_UP_A_GAME.md).

## Exit codes
**0** done (or dry run finished). **1** the new game failed `check_manifest` (this is a bug; the files stay for inspection).
**2** cannot run: missing file or folder, bad id, id or UID3 taken, triage failed (its message and last output lines are
shown), or the folder could not be written.

## Errors
| message | meaning |
|---|---|
| `... is not a file` / `... is not a folder` | wrong path for the game or a lib folder |
| `cannot make an id from ...` | the file name has no letters or digits: give `--id` |
| `games/<id> exists already` | pick another `--id`, or `--force` while pending |
| `UID3 ... already belongs to games/<other>` | see "Another build" above |
| `triage failed (exit N)` | the pipeline stopped; nothing was created in `games\` |

## Tests
`tools\tests\test_newgame.py` runs the script on a synthetic game in a scratch project: the created files and their check,
dry run, `--force` on a pending and on an accepted game, the profile that must stay untouched, the UID3 clash and the bad
inputs. The profile stub has its own tests in `test_newgame_tools.py`.
