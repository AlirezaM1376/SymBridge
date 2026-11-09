# Bringing up a new game

This is the path from "I have an old S60 game" to "it is a permanent regression test". Each step says what you run and
what you should see. Commands are shown for Windows; on Linux use `/` in the paths.

Everything the tools need is in the repository except two things you bring yourself: the game's `.app` (never commit it)
and the SDK libraries (S60 v2 libs and S60 v3 `.dso`/`.lib` files.

## 0. One-time setup
- Python 3.8 or newer (no extra packages).
- `c++filt` from a modern binutils (needed for the v3 names; `--no-cxxfilt` works but gives a much worse map).
- The S60 v2 SDK `lib` folder (EKA1 `.lib` files) and the S60 v3 `epoc32\release\armv5\lib` folder (`.dso`or `.lib` files).
- Check that the repository is healthy: `python tools\check_all.py` must say PASSED (unit tests may fail, but if everything else passed you can ignore it).

## 1. Triage the game
```
python tools\newgame.py C:\work\MyGame.app --v2-libs C:\sdk\v2\lib --v3-libs C:\sdk\v3\lib --name "My Game"
```
This runs the whole static analysis and prints a one-page triage report (also saved in `work\MyGame.triage.md`). Read it
before going further:
- **THUMB code** or framework classes the host does not know are the usual things that may need more time.
- The predicted shim / direct / trap counts tell how much of the game's imports the host handles. Traps are only a problem
  when the game really calls them.
- The `display_mode` in the new profile is only a **guess**. It must be the screen mode the game draws for. If the game runs
  but the screen stays white or black, set `display_mode` in `profiles\<UID3>.prf` (RedHood needs 7; the cookbook, mistake 25).

It also creates, without touching anything that exists:
- `games\mygame\map.csv` and `games\mygame\game.json` (status `not_tested`; the game is "pending" until step 5)
- `profiles\<UID3>.prf` with the screen size and display mode the analysis guessed, everything else commented out

## 2. Add the game to the host
```
python tools\gen_importmap.py
```
This merges the new map into `src\importmap_data.cpp`. Rebuild the host and the SIS. The profiles folder is packaged to
`c:\system\apps\symbridge\profiles\` on the phone, so the new `.prf` travels with the SIS.

## 3. First run on a phone
1. Copy the game folder (with the `.app` and its data) to `E:\symbridge\<GameFolder>\` on the phone.
2. For the first run put `link_report=1` in the game's profile. The host then writes `<app>_links.txt` next to the game with
   one line per import and what happened to it.
3. Start SymBridge, pick the game. The host writes its log and, in the game folder, a `configs.cfg` with the user settings
   (scaling, FPS counter, softkeys). Edit the profile, not `configs.cfg`, for facts about the game (screen size, display
   mode, stack, exit trick) `logging` in `configs.cfg`: 0 nothing, 1 the normal log (default), 2 detailed (slower).
4. If the game does not start, crashes or does not stop: read the log (set `logging=2` in `configs.cfg` for a crash or a
   freeze, see "If something fails"). The `link:` lines show the import counts, `hits:` says whether
   a trap was called, `thread:` / `exit:` lines show how it ended. Games that never stop on their own need the exit trick
   keys in their profile (see `profiles\default.prf`).
5. Copy the log back to the PC.

## 4. Accept the run
When the game runs well:
```
python tools\accept.py mygame C:\logs\MyGame_phone.log --status plays
```
`accept.py` refuses when the log is from another game, the run did not end normally, or the numbers predicted from
`map.csv` differ from the log (which means the map is not the version that ran, or `gen_importmap.py` was not run before the
build). On success it writes `games\mygame\baseline.log` (the reduced log, made by `mkbaseline.py`) and the expected numbers
and status into `game.json`. Use `--status partial` or `menu` when it only half works.

## 5. Make it permanent
```
python tools\coverage_all.py
python tools\check_all.py
```
`coverage_all.py` refreshes `docs\COVERAGE.md`; `check_all.py` must pass. Commit `games\mygame\`, `profiles\<UID3>.prf`,
`src\importmap_data.cpp` and `docs\COVERAGE.md` together.

## Another version of the same game
Different releases of one game often differ by a few imports. Keep the version that matches your log as `map.csv`; put the
other as `map.alt.csv` in the same folder and run `python tools\mkmanifest.py games\<id>\baseline.log` to list it in
`game.json`. Add a note saying what differs.

## If something fails
| message | meaning |
|---|---|
| `importmap up to date: FAIL` | a map changed; run `python tools\gen_importmap.py` |
| `COVERAGE.md up to date: FAIL` | data changed; run `python tools\coverage_all.py` |
| `manifest: FAIL ... expect.trap=...` | `game.json` and `baseline.log` disagree; re-run `accept.py --update` with the right log |
| `coverage <id>: FAIL` | the map no longer predicts the logged counts: a registry or direct-bind change moved a game |
| `PENDING <id>` | the game has no accepted run yet; not an error |

### Reading a crash
The game dies with a panic and the log says nothing (for example `KERN-EXEC 3`), or the phone freezes. Set `logging=2` in the
game's `configs.cfg` and run again. At level 2 the log is flushed after every line, so it survives a freeze, and after the
`CRASHWATCH:` line it lists the last 64 calls the old code made into the host:
`CRUMB n: tick .. tag .. thread .. a=.. b=.. lr=..`.
1. Read the crumbs from the **bottom**: the last one is the last thing the game did before it died.
2. `lr` is a return address in the old code. Subtract the chunk address in the log line `game: chunk 01800000 ...` to get the
   offset in the `.app` (`0180ed74` - `01800000` = `0xed74`), then disassemble the `.app` at that offset.
3. Tags 21/22 are the start and end of one game frame: a 21 without a 22 means the game died inside that frame. 10/11 are the
   start and end of a present. The full list is in `crashwatch.h`.
4. A frame that dies right after a `Mem::Copy` row loop (tags 1-3) is usually a bad source pointer or **an uninitialised
   object** (cookbook, mistake 24); `a` and `b` are the destination and the length.
5. Level 1 (default) records only the cheap crumbs (no tick, no thread id) and level 0 none. A crash after a clean exit shows
   `type=0 ... reason=0`; that is not a crash.
   
## Games with their own menu
Some games (RedHood, OwnGoal) have an Eikon menu in their `.rsc`: the left softkey opens "Options", the right one is Exit/Cancel. The
old framework built that menu; the host does not. What the host can do today is send **one command per softkey**: look in
the game's `.rsc` for the command ids and put them in `configs.cfg` (`softkey_left_cmd`, `softkey_right_cmd`; `0` = unused).
The rest of the menu is not reachable. Note it in the game's `game.json` notes and in the README table.
 
## Test on a second phone
Phones differ in ways old games notice: a game that runs on an S60 3rd phone can die on a Symbian Belle phone because Belle
reuses freed heap cells (cookbook, mistake 24), and a crash can be a clean panic on one and a frozen phone on the other. When
you can, run a new game on two OS versions before you accept it, and say which in the `game.json` notes.