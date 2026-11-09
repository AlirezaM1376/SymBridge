# SymBridge

**Run old Symbian 6,7 and 8 apps (focused on S60 v1/v2 games) on Symbian 9.x (S60v3, S60v5, S^3, Anna and Belle) phones.**

SymBridge is a runtime or compatibility layer. Games written for S60 1st and 2nd Edition are native ARM binaries built for the old EKA1 kernel and an old compiler.
They cannot load on a Symbian 9 phone: the executable format, the system calls and the C++ ABI are all different. SymBridge is a native
S60 3rd Edition application that **loads the old binary itself**, relocates it, and answers every system call the game makes
with its own code or with the real S60 3rd Edition function. The game runs as it did on the old phone, on the new hardware, with
its own scaling, key handling and sound.

> **Status:** early, but working. 25 games play; see [Supported games](#supported-games) for the list.
> The project is a hobby preservation effort. Expect rough edges.
> While 90% of the code-base written by AI, the other parts including plannings, code reviewing, buldings, and testings done manually by human.

## Screenshots

![](images/RedHood_E7.png "RedHood on Nokia E7")
![](images/Snails_N86.png "Snails on Nokia N86")
![](images/Qwak_603.png "Qwak on Nokia 603")

## How it works

SymBridge does not emulate a CPU. The game's ARM code runs natively; the host replaces everything the game *touches*.

```
game (.app) -> legacyimage -> iatfill -> [ direct | shim | trap ] -> real S60 3rd Ed
       |            ^ relocated, runs natively        ^ host-made old-style vtables and object facades
       |
       |  every import of the game (dll, old ordinal) is bound once, at link time, to one of:
       |
       +-- direct : the real S60 3rd Edition function (plain integer/pointer functions only)
       +-- shim   : a function of the host that translates (old object layouts, old ABI, old semantics)
       +-- trap   : a stub that logs the call; it only matters if the game really calls it
```

0. **Static analysis first.** Before a game is ever run, Python tools predict how many of its imports the host handles and which
  unresolved ones are actually called (`tools/triage.py`).
1. **Load.** The host parses the old E32Image itself, copies it into a code chunk and relocates it.
2. **Bind.** Every import `(dll, old ordinal)` is bound once to a **direct** real S60 3rd Ed function (only when signature, ABI and layout are identical), a **shim** (a host function that translates old object layouts, GCC 2.95 calling convention, old semantics) or a **trap** (logs the call, returns 0).
3. **Run.** The host enters the old code through a small runtime of its own (old code has no unwind tables, so `TRAP`/`Leave` are rebuilt) and plays the old framework's part: application, document and AppUi objects, active scheduler, threads, graphics, keys.
4. **Show.** The finished frame is copied into a real S60 3rd Ed control and scaled; keys and softkeys travel the opposite way.

The key modelling rule: **the game owns the bytes, the host owns the class.** Old code allocates its own objects and reads
their fields inline, so the host works *inside* those bytes (descriptors, `TParse`, arrays), or points them to host-made
old-style vtables (`CActive`, bitmap, gc), or pairs them with a real v3 object in a side table (scheduler, audio, threads).

Full description: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Features

- Ability to load S60 v1/v2 application images (ARM, no writable static data) from phone memory, internal mass storage or memory card
- Screen scaling: original size, fit with aspect ratio (nearest or bilinear), full screen (nearest or bilinear), cycled with a hot key
- Key mapping and softkey-to-command mapping per game
- Sound through the old `CMdaAudioOutputStream` API, played by the real S60 3rd Edition audio stream
- FPS counter and an optional frame-rate limit for games with no timing of their own
- Per-game profiles (screen size, thread stack, exit behaviour etc.) and a user config file next to each game
- A log file with one line per decision, written for diagnosing a game on a real device
- Comaptible with S60v3, S60v5, S^3, Anna and Belle phones

## Supported games

Each game's analysis, import map and reduced device log are in [`games/`](games/); the numbers are summarised in
[`docs/COVERAGE.md`](docs/COVERAGE.md). In short:

<!-- Fill in the Status column from games/*/game.json before publishing. -->
| Game | Status |
|---|---|
| Amiba's Candy | Plays |
| Another World | Plays (only version 1.25 of the game) |
| Aqua Blocks | Plays |
| Aqua Blocks Plus | Plays |
| Atomanic | Plays |
| Cannon Fodder | Plays |
| Cannons | plays |
| Captain Dynamo | Plays |
| Crystal Cavein | Plays |
| Fireball | Plays |
| Hell Striker | Menus |
| Jewel Master | Plays |
| MagicPuz | Plays |
| Nebulus | Plays |
| OwnGoal | plays; Options menu not reachable, left softkey for new game |
| Qwak | Plays |
| RedHood | plays; Options menu not reachable, left softkey for new game |
| Rising Diamonds | Plays |
| Rodland | Plays |
| Sapphire Spires | Plays |
| Snails | Plays |
| Snake | Plays |
| Stone Pilot | Plays |
| Stone Stacker | Plays |
| Stormlord | Plays |
| Sunstone Swap | Plays |

**The game binaries are not part of this repository and never will be.** You need to own a copy of each game and put its data on
your phone yourself. Symbian S60 v1/v2 games archives are available online, one can be found here: https://mega.nz/folder/roBW3DxR#0k0B9kFLdmjwXoBP_DqLQQ

## Requirements

**To run it**
- A Symbian phone with one of these platforms: S60v3, S60v5, S^3, Anna or Belle
- The SIS file of the SymBridge, installed like any other application
- The game files (extracted from game .sis package), each game with its own folder, all placed inside !:\SymBridge (! can be one of E/F/C drives)
- For extracting game data from its .sis package you can use SISWare Windows program.

**To build it**
- S60 3rd Edition Feature Pack 1 SDK (S60v3 FP2 should be fine too but not tested), with the GCCE toolchain
- Python 3.8 or newer for the tools (no extra packages)
- `c++filt` from a modern binutils (for the tools only)
- To analyse a *new* game: the S60v2 FP1 SDK `lib` folder and the S60v3 FP1 SDK `.dso` or `.lib`  files (obtain them yourself, SDks are available online)

## Using it

1. Put the game folder on one of the E/F/C drives as `\symbridge\<any name>\`, containing the game's `.app` and its data files.
2. Start SymBridge. With one game folder it starts that game. With several, write the folder name (or the UID3 as 8 hex
   digits) on the first line of `\symbridge\autostart`; without that file the first folder alphabetically is used.
   Commenting names in autostart file works; put a # before name of the game folder.
3. On the first run of each game, SymBridge writes `configs.cfg` into the game folder. It holds the user settings:

| key | meaning |
|---|---|
| `scale_mode` | 1 original, 2 fit (aspect), 3 fit + bilinear, 4 full screen, 5 full screen + bilinear |
| `scale_key` | key that cycles the scale mode (default: Green/Call key) |
| `show_fps` | `1` shows an FPS counter |
| `max_fps` | `0` = unlimited; `30`..`50` slows down games with no timing of their own |
| `softkey_left_cmd`, `softkey_right_cmd` | command id sent to the game when a softkey is released (`0` = unused) |
| `logging` | `0` short log file, almost nothing, `1` writes basic logs (default),  `2` detailed log: crash flight recorder, flush every line (slower) |

Facts about a *game* (screen size, stack size, how it is stopped and more) live in profiles, not in `configs.cfg`:
`c:\system\apps\symbridge\profiles\default.prf` and `<UID3>.prf`, installed by the SIS from [`profiles/`](profiles/).

## Repository layout

| folder | contents |
|---|---|
| `data/` | SymBridge's Symbian resource files |
| `src/` | the SymBridge: loader, linker, old runtime, shims, GUI |
| `group/` | project files for the S60 3rd Edition SDK |
| `profiles/` | per-game profiles, packaged into the SIS |
| `games/` | one folder per game: import map, manifest, reduced device log (no binaries) |
| `tools/` | analysis, bring-up and check scripts (Python) and their tests |
| `docs/` | documentation, see below |

## Documentation

| document | read it when |
|---|---|
| [`docs/BRINGING_UP_A_GAME.md`](docs/BRINGING_UP_A_GAME.md) | you want to add a new game: from `.app` to a permanent regression test |
| [`docs/SHIM_COOKBOOK.md`](docs/SHIM_COOKBOOK.md) | you must write a shim: patterns with skeletons, real examples and the usual mistakes |
| [`docs/LAYOUT_FACTS.md`](docs/LAYOUT_FACTS.md) | something looks like a wrong object or offset: every hard-coded old-layout number and how it was checked |
| [`docs/COVERAGE.md`](docs/COVERAGE.md) | you want to know which imports are handled for which game |
| [`docs/tools/`](docs/tools/) | one page per tool (`triage`, `newgame`, `accept`, `check_all`, ...) |

## Tools

All tools are plain Python (3.8+) and share the same exit codes: **0** ok, **1** a check failed, **2** the tool could not run.

```
python tools/newgame.py  GAME.app --v2-libs <dir> --v3-libs <dir> --name "My Game" [--cxxfilt <path to cxxfilt>]   # analyse and add a game
python tools/layout_check.py GAME.app --map games/<id>/map.csv                       # does it match the documented layouts?
python tools/accept.py <id> device.log --status plays                                # make a good run a regression test
python tools/check_all.py                                                            # repository health, must pass before a commit
python -m unittest discover -s tools/tests -t tools/tests                            # unit tests
```

## Contributing

- **AI usage:** You can use AI for vibe-coding, however your AI generated code should be reviewed and tested. Do not pull request withot enough test and code review because I will not accept it.
- **Read/Check (Again, feel free to use AI) [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md), then [`docs/SHIM_COOKBOOK.md`](docs/SHIM_COOKBOOK.md), then [`docs/LAYOUT_FACTS.md`](docs/LAYOUT_FACTS.md).**
- **A new game:** follow [`docs/BRINGING_UP_A_GAME.md`](docs/BRINGING_UP_A_GAME.md). A pull request should contain the game's
  folder under `games/`, its profile, the regenerated `src/importmap_data.cpp` and a device log (Pull request for a new game will be accepted only when it's playable).
  Never include the game binary or any file from a SDK.
- **A new shim:** check patterns in [`docs/SHIM_COOKBOOK.md`](docs/SHIM_COOKBOOK.md), copy skeleton if a pattern match your target (or write a new one if neccessory), comment the table
  row with the old C++ name, and run `python tools/check_all.py`.
- **Bug reports:** attach the log (`SymBridge_gui.log`), the game name and version (UID3 and file size), and the phone model.

## Known limits

- No user-friendly GUI for now, everything is minimal.
- Only ARM code is loaded (no Thumb), and only images without writable static data.
- Only imports listed in the registry are handled; the rest are traps. A game that calls a trap is stopped or gets 0, depending
  on the mode; the log says which import it was.
- Audio uses `CMdaAudioOutputStream`. Other audio APIs (for example the media-client player utility) are not implemented yet.
- Sound, speed and key behaviour depend on the phone. Games that never wait for the clock may need `max_fps`.
- Games with their own Eikon menu (RedHood: left softkey = Options) work, but the menu is not shown: only one command per softkey (softkey_left_cmd / softkey_right_cmd) can be sent.
- Masks / alpha of bitmaps are not implemented (bitmap load goes through the real decoder only for the colour part).
- Games that start other processes (RProcess) are not supported.
- Tested on E5 (Symbian S60v3 FP2) and E7 (Symbian Belle); behaviour of the old heap differs (zeroed allocation rule).

## License

MIT; See [`LICENSE`](LICENSE)

SymBridge is not affiliated with or endorsed by Nokia, Symbian or the publishers of any game. All trademarks and game titles
belong to their owners.
