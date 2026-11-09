# Architecture

How SymBridge runs a binary that was built for another operating system, and how the host models the old world.

## 1. The problem

An S60 1st/2nd Edition game is a native ARM image for the **EKA1** kernel, built with **GCC 2.95**. A Symbian 9 phone
cannot run it, for four independent reasons:

| layer | old (S60 v1/v2) | new (S60 3rd Ed, Symbian 9) |
|---|---|---|
| executable | E32Image, ER5 format, relocated by the loader | a newer E32Image variant (ELF-based tool chain, platform security) |
| system calls | imports by **ordinal** from `euser`, `eikcore`, `fbscli`, ... | different DLLs, different ordinals, some functions gone, some inline |
| C++ ABI | GCC 2.95: `this` in r0, constructors return `this`, destructors take flags, doubles as two words (high first), structs by hidden pointer, 2-word vtable header | ARM EABI / RVCT-GCCE |
| objects | old class layouts (`CBase`, `CActive`, `CFbsBitmap`, `TParse`, ...) | different sizes, different member order, different vtables |

SymBridge does not emulate a CPU. The old code is ARM and the phone is ARM, so the game's instructions **run natively**. What
has to be replaced is everything the game *touches*: the loader, every import, every object whose layout the game knows.

## 2. The idea in one picture

```
 +--------------------------- SymBridge (a normal S60 3rd Ed application) ----------------------------+
 |                                                                                                      |
 |  gamelocator -> profile + configs.cfg -> legacyimage -> iatfill -> oldrt -> GAME CODE (native ARM)   |
 |   (find game)    (facts / user prefs)    (load+reloc)   (bind all     (enter/leave        |   ^       |
 |                                                          imports)      old code)          |   |       |
 |                                                                                  imports  v   | returns|
 |        +--------+ direct: real v3 function --------------------------------------------------+       |
 |        | shims: memshim compatshim fsshim schedshim activeshim threadshim audioshim rtshim ...       |
 |        | fakes: eikshim ctlreg  (fake CCoeEnv, application, document, AppUi)  gfxfake (fake gfx)     |
 |        | trap : logs the call, returns 0                                                             |
 |        +---------------------------------------------------------------------------------------------+
 |                                                  |                                                   |
 |  lhgui (real CAknAppUi + control)  <-- frame / key / foreground --  swscale (scaling)                |
 +------------------------------------------------------------------------------------------------------+
```

The game believes it runs inside the old framework. In fact it runs inside a real S60 3rd Ed application (`lhgui`) that
**plays the old framework's part** towards the game and the new framework's part towards the phone.

## 3. Life of a game

1. **Locate** (`gamelocator`). Scan `\symbridge\` on `E:`, `F:`, `C:` for a folder with an application image (`EPOC`
   signature, UID2 = application). `\symbridge\autostart` chooses one if several exist.
2. **Configure** (`profile`, `hostconfig`). `profiles\default.prf` + `profiles\<UID3>.prf` carry *facts about the game*
   (screen size, display mode, stack, exit behaviour). `configs.cfg` next to the game carries *user preferences* (scaling,
   FPS, softkeys, `logging`). They are kept apart on purpose: a fact is shared by every user of the game.
3. **Load** (`legacyimage`). Parse the old E32Image: header, code region (text + import address table + export directory),
   import blocks, relocation pages. Copy it into a code chunk and apply the relocations.
4. **Bind** (`iatfill`). For every import `(dll, old ordinal)` write one address into the game's **import address table**.
   The address is one of three things (section 4). Done once, at link time; there is no lookup while the game runs.
5. **Start** (`legacygame`). Call export 0 (`NewApplication`) through `oldrt`; then application word 14 `CreateDocumentL`,
   document word 19 `CreateAppUiL`, AppUi word 15 `ConstructL` - exactly the sequence the old framework performed. Every
   one of those objects is allocated by the game itself, inside its own heap cells.
6. **Run**. The game builds its controls, starts a `CIdle`/`CPeriodic`/`CTimer` frame loop or its own threads. All of that is
   served by `schedshim`, `activeshim`, `threadshim` on top of real v3 active objects and threads.
7. **Present**. The game draws into what it believes is a `CFbsBitmap`/window gc. `gfxfake` copies the finished frame to
   `lhgui`'s control, `swscale` scales it, and the real window server shows it.
8. **Input**. Real key events arrive at `lhgui`. The host forwards them the way the old framework would have:
   `AppUi::HandleKeyEventL` if the game overrides it, otherwise `OfferKeyEventL` of the controls the game registered
   (`ctlreg` captures `AddToStackL` and the foreground observers from the game's own calls).
9. **Exit**. Games that never end are stopped by the *exit trick* from the profile; `crashwatch` reports how every thread
   ended and dumps the flight recorder after a panic.

## 4. The three kinds of binding

| kind | what the game gets | when it is right |
|---|---|---|
| **direct** | the address of the real v3 function | only if signature, calling convention, class layout and semantics are identical, and nobody calls back into old code. A list in `directbind.cpp`, checked by `tools/check_direct.py` (rules R1-R5) |
| **shim** | the address of a host function | everything else that the game really uses. Written by hand, registered in a table, see [SHIM_COOKBOOK.md](SHIM_COOKBOOK.md) |
| **trap** | a stub that logs `(dll, ordinal)` and returns 0 | the default for the rest. Costs nothing unless called; when called, the log says exactly what to write next |

The static map `(game, dll, ordinal) -> v3 name` comes from the game's `map.csv`, generated by the Python tools from the S60 v2
`.lib` and S60 v3 `.dso` files; `tools/gen_importmap.py` merges them into `src/importmap_data.cpp`. At run time the host
asks the **shim registry** (`shimreg`, ordered by registration) before it looks at direct binds, so a shim always wins.

This is the main loop of the project: *run the game -> read the trap in the log -> write the shim -> run again*
([BRINGING_UP_A_GAME.md](BRINGING_UP_A_GAME.md)).

## 5. Modelling the old world

### 5.1 The old ABI (`oldrt`, `rtshim`, `compatshim`)

Rules every shim obeys (full list: cookbook, section 1):

- **Calls into the host** use the old convention, which matches EABI for plain integer/pointer signatures. For doubles, the
  host reads two words in old order (`r0` = high word), and returns the same way. 64-bit integers are packed without the
  8-byte alignment EABI wants. Structs larger than 4 bytes come back through a hidden pointer.
- **Constructors return `this`; destructors are called `dtor(this, flags)`**, bit 0 = also free the object. Vtables have two
  header words, so the first virtual is word 2 (for classes derived from `CBase`: the destructor).
- **Leaving.** Old code has no unwind tables, and a v3 C++ exception must never travel through old frames. The host therefore
  has its own tiny runtime, written in hand-assembled ARM: a setjmp/longjmp-style trap frame per `TRAP`, and **one** way out of
  old code, `TOldRt::Leave`. Old code is only entered through `TOldRt::Call0/Call1/CallArgs`, which also keep the per-thread
  state (`TOldThreadState`: trap chain, cleanup depth, TLS, "is a game thread").
- **Callbacks.** Anything that calls old code (a `CIdle` callback, `RunL`, a thread entry, `qsort`'s comparator) goes
  through `TOldRt`, never straight. This is why callback-taking functions can never be direct binds.

### 5.2 Objects: the host owns the *class*, the game owns the *bytes*

The game allocates its objects itself (`new`, `User::AllocL`, on its stack, as members of its own classes) and knows their
size and the offsets of the fields it reads inline. The host cannot change any of that. So for every old class the host uses
one of these models (cookbook, patterns 3, 4, 9, 10):

| model | idea | examples |
|---|---|---|
| **plain function** | no object at all | `Mem::Copy`, `User::After` |
| **in-place facade** | the host works *inside* the game's bytes, using a layout it knows (and records in [LAYOUT_FACTS.md](LAYOUT_FACTS.md)) | descriptors (`TPtr8`, `TBuf`), `TParse` (stateless: rebuilds a real `TParse` from the name buffer on every call), `CArrayFixBase` (count at +4, size 24 verified; the rest chosen by the host) |
| **facade + vtable** | the game's object carries a pointer to a **host-made old-style vtable** whose slots are host functions | `CActive` (24 bytes: vptr, iStatus, link, iActive), fake `CFbsBitmap`, fake gc, fake `CCoeEnv` |
| **paired object** | the game's object is a thin facade; the *real* v3 object lives beside it in a side table and does the work | `CActiveScheduler`, `CIdle`, `CPeriodic`, the audio stream, `RThread`/`RSemaphore` |
| **handle wrapper** | the old `R...` class holds a handle; the shim forwards to the real v3 class with the same handle | `RFile`, `RFs` |
| **host object** | one object serves the whole process | the application, document and AppUi framework |

Two facts about this layer turned out to matter more than expected:

- **Layouts are verified, never guessed.** Each size/offset in `LAYOUT_FACTS.md` is marked as *general* (true for all tested
  games), *per game* (a profile key), or *host choice* (nothing in the game depends on it).
- **Old heap memory is zero.** Old games read memory they never wrote. The old heap handed out fresh zeros, S60 3rd reuses
  freed cells. Every allocation shim therefore returns **zeroed** memory (`User::AllocZ`).

### 5.3 The framework fakes (`eikshim`, `ctlreg`, `lhgui`, `legacygame`)

The game is an *application*: it derives from `CEikApplication`, `CEikDocument`, `CAknAppUi`, and calls `CCoeEnv::Static()`.
The host models this like so:

- the **base class constructors** (`eikcore`/`avkon`) are shims that hand back `this`: the game's objects are allocated
  zeroed, and most of the base state is not needed (the base sizes are general: application 556, AppUi 96);
- the **real** `CAknAppUi`, container control and window belong to `lhgui`, and the old AppUi/document are *called by*
  the host (`ConstructL`, key, foreground, window-server events) the way the old framework would call them;
- **what the game registers** is learned from its own calls, not from offsets: `ctlreg` records `AddToStackL` and foreground
  observers; whether an AppUi virtual is overridden is detected by checking if the vtable word is one of the image's own
  import thunks (an inherited framework function);
- `CCoeEnv::Static()` returns a **fake CCoeEnv** with the few fields old code reads inline (`+0x3c` = screen, `+0x18` = AppUi).

### 5.4 Graphics (`gfxfake`, `lhgui`, `swscale`)

Old games draw with `CFbsBitmap`, `CFbsBitGc`, `CWsBitmap`, `CWindowGc`. Their layouts differ completely in v3, and the old
inline code reads the bitmap's fields directly. So:

- A **fake bitmap** (0x18 bytes: pixels +8, width +0xc, height +0x10, mode +0x14, with its own vtable) is what the game gets.
- **Loading** a game's `.mbm` uses the phone's own decoder: real `CFbsBitmap::Load` + `GetScanLine` into the fake bitmap
  (this needs an `RFbsSession` per game thread). There is no home-grown decoder.
- **Contexts** are fake gc objects with a host vtable; those bound to a bitmap device draw into it, others return 0
  (the first calls are logged, so a missing word shows up immediately).
- **Presentation**: when the game's frame is done, the host copies it to the screen buffer only if the profile's
  `display_mode` (the screen mode the game expects) matches the bitmap mode. `swscale` then scales (original, fit,
  full screen, nearest or bilinear) in fixed point on RGB565.
- **Paths**: `\system\apps\<game>\...` and drive-letter paths are remapped into the game folder (`fsshim`), including for the
  graphics loader.

### 5.5 Time, threads and sound

- **Schedulers and active objects** (`schedshim`, `activeshim`): the game's `CActive`/`CIdle`/`CPeriodic` objects are
  facades for real v3 active objects, whose callbacks enter the old code through `TOldRt`.
- **Threads** (`threadshim`): an old `RThread` becomes a real v3 thread whose body runs old code. Semaphores, mutexes and
  TLS follow. Game threads share the creator's heap (they are created with no own allocator), and the old
  exceptions-as-signals (`SetExceptionHandler`/`RaiseException`) are emulated with an active object in the target thread.
- **Sound** (`audioshim`): the old `CMdaAudioOutputStream` API is mapped to the real v3 stream; every v3 leave is trapped
  at the boundary. A volume ratio compensates for the louder v3 hardware.

## 6. Verification

The part of the project that makes it more than a pile of hacks:

- **Static analysis before running.** `tools/newgame.py` computes how many imports of a game will be direct, shim and trap, and
  which unresolved ones the code actually reaches. `check_direct.py` enforces the direct-bind rules. `layout_check.py` verifies
  layout claims against a binary.
- **Regression without a phone.** Every accepted game has `games/<id>/baseline.log` (the reduced device log) and `game.json`
  (expected numbers). `tools/check_all.py` recomputes the predictions from the maps and registry and compares them with the
  baselines: a registry or direct-bind change that silently moves a game is caught on the PC.
- **A log written for diagnosis.** `logging=1` keeps one line per decision. `logging=2` adds a crash flight recorder: the last
  64 calls from old code into the host (tag, arguments, return address); after a panic, return address minus the logged
  `game: chunk` address is the offset in the `.app` to disassemble ("Reading a crash" in BRINGING_UP_A_GAME).

## 7. What this is not

- Not a CPU emulator: THUMB-only images and self-modifying tricks are out of scope; code is ARM and runs natively.
- Not a general compatibility layer: shims exist for what the supported games use. A new game usually needs a few new shims.
- Not complete: see "Known limits" in the README.

## 8. Map of the source

| module | role |
|---|---|
| `gamelocator`, `profile`, `hostconfig`, `hostlog` | find the game, facts, user config, log |
| `legacyimage`, `iatfill`, `importmap(_data)`, `shimreg`, `directbind` | load, relocate, bind |
| `oldrt`, `rtshim`, `compatshim` | entering/leaving old code, ABI helpers, small pure shims |
| `memshim`, `fsshim` | heap, cleanup stack, files, paths |
| `schedshim`, `activeshim`, `threadshim` | scheduler, active objects, threads |
| `eikshim`, `ctlreg`, `legacygame`, `lhgui` | framework fakes, game lifecycle, the real UI |
| `gfxfake`, `swscale`, `audioshim` | graphics, scaling, sound |
| `crashwatch` | thread watchdog and flight recorder |
