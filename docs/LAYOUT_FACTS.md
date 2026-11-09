# Layout facts

Every hard-coded number in the host that describes how an **old (S60v1/v2, GCC 2.95)** object, vtable or structure looks:
what it is, where it is in the code, where it comes from and how it was checked. When a game breaks and the log looks like
"wrong object", "jump to 0" or "a value that makes no sense", this is the first list to check. When a **new** game is added, this
list says which facts are general and which were only ever seen in one game.

## How to read the tables

| status | meaning |
|---|---|
| **BIN** | checked in the binaries of the games in `games\`, with `tools\layout_check.py` (see the last section) or step0's vtable analysis. The number of games is given. |
| **RUN** | not read from a binary, but the host works on the device with it (`games\*\baseline.log`). A wrong value would show. |
| **SDK** | taken from the v2 SDK headers (named in the code comment). Not re-checked here. |
| **CODE** | stated only in a code comment, no other evidence recorded. Treat as an assumption. |
| **GAME** | depends on the individual game; comes from a profile key, not a constant. |

Vtable numbering everywhere: **word n** = `vptr[n]`. Words 0 and 1 are the header, **word 2 = destructor**, virtual functions
follow from word 3 in declaration order, base class first. (`ctlreg.cpp:VtOf`, `legacygame.cpp:TLegacyGame::VTable`, the same
numbering as step0's `own`/`slot_imports`.)

## 1. What is general and what is not (read this first)

| fact | general? |
|---|---|
| Sizes of `CEikApplication` (556) and `CEikAppUi` as derived by the game (100, so the framework base is 96) | **general**: because the games add no members to the application and one pointer to the AppUi |
| `CEikDocument` 36, `CFbsBitmap` 24 | general |
| AppUi words 15 (`ConstructL`), application word 14 (`CreateDocumentL`), document word 19 (`CreateAppUiL`) | general |
| AppUi word 19 (`HandleCommandL`) | **not general**: Candy does not override it (the slot is an import thunk). Only used with `softkey_*_cmd` |
| AppUi words 8 / 9 / 3 (`HandleKeyEventL`, `HandleForegroundEventL`, `HandleWsEventL`) | slot numbers general; **whether the game overrides them is per game** and the host asks (`ctlreg.cpp:CtlReg_AppUiOverrides`) |
| Control word 3 (`OfferKeyEventL`) | slot number general; AnotherWorld has no control that overrides it (it uses the AppUi route) |
| `CCoeEnv` +0x3c = `iScreen` | general (games read it right after `CCoeEnv::Static()`) |
| `CCoeEnv` +0x18 = the AppUi | general, see the `CCoeEnv` +0x18 row in section 2 |
| Exit state: `tls+0x60` -> block, state word at `+0x20` = 3 | **Captain Dynamo and similar games by same developer**. It is the *default* of the profile keys, not a fact about old games (see 5) |

## 2. Framework object layouts (old side)

| fact | where | source | status | what breaks if wrong |
|---|---|---|---|---|
| `CEikApplication` is **556 bytes**; `iProcess` at **+20** | `eikshim.cpp:ShimAknDocCtor` (reads `app+20`, copies it to document +12) | size: code comment "verified"; now re-checked as the argument of `CBase::operator new` before `CEikApplication::CEikApplication()` | size **BIN**; the +20 offset **CODE** (verified on Captain Dynamo per the comment, not re-checked) | the document gets a wrong `iApaProcess`; games that read it crash |
| `CEikDocument` is **36 bytes**: vptr 0, iContainer 4, iApplication 8, iApaProcess 12, iSpare 16, iAppUi 20, iEditStore 24, iChanged 28, iAppFileMode 32 | `eikshim.cpp` (comment above `ShimAknDocCtor`) | allocation size of the document in the game | size **BIN** (in Rodland no allocation was recognised; in 6 games the evidence is an unnamed 36-byte allocation, in Candy the allocation is followed by the document constructor); the field names **CODE**; the host writes only +8 and +12 | writing +8/+12 into a differently laid out document corrupts it |
| `CEikAppUi` as derived by the game is **100 bytes**; the framework base (`CAknAppUi`) is therefore **96** | not in the code as a number; `ctlreg.h` comment: "container at appui+0x60" (= 96, the first game member) | allocation size before `CEikAppUi::CEikAppUi()` | **BIN** (100 in every game) | nothing reads it today. It matters if a shim ever stores state *inside* the old AppUi: the game's own members start at +96 |
| `CCoeControl`: `iCoeEnv` at **+8** | `lhgui.cpp:ShimCoeControlCtor` | "Only sets what old code reads inline" | **RUN**; the games' own control sizes differ (52..60) | `CCoeEnv::Static()`-less code reads a NULL env |
| `CCoeEnv`: **+0x3c = `iScreen`** (`CWsScreenDevice*`) | `lhgui.cpp` (set after `GfxFake_Init`, comment "v2 offset 0x3c") | comment; now also read in the binaries: `bl CCoeEnv::Static()` followed by `ldr rN,[r0,#60]` | **BIN** | games draw to a NULL screen device |
| `CCoeEnv`: **+0x18** (24) = the AppUi | `lhgui.cpp:gFakeEnv` (256 zero bytes; +0x18 is **not** set) | `bl CCoeEnv::Static()`, `ldr r0,[r0,#24]`, `bl CEikAppUi::Application()` in game binaries (not Candy) | **BIN** (the access pattern); the field's meaning is inferred from the next call | works only because `ShimApplication` (eikcore 17) ignores `this`. A shim that *uses* the AppUi pointer it is given would see 0 |
| `gFakeEnv` is 256 bytes, zeroed | `lhgui.cpp:gFakeEnv` | size chosen generously, the old `CCoeEnv` was smaller | **CODE** | a game reading past 256 bytes of the env |
| Old `CActive`: vptr 0, iStatus **+4**, iLink 8..19, iActive **+20**, size **24**; the game's members follow | `activeshim.cpp:KOldActiveSize`, `ShimActiveCtor` (zeroes +4..+23, keeps the vptr) | code comment ("GCC 2.95, EKA1") | **CODE**. Slots (below) are BIN | inline code reading `iStatus` or `iActive` sees garbage if the ctor shim did not zero it |
| `CActive` slots: **3 DoCancel, 4 RunL, 5 RunError** (2 = dtor) | `activeshim.cpp:KSlotDoCancel/KSlotRunL/KSlotRunError` | vtable analysis of the games' `CActive`/`CIdle` subclasses: `CActive::RunError` is an import at slot 5 and the game's own words are 2,3,4 | **BIN** (Candy, Atomanic) | the real active object calls the wrong game function |
| Fake `CIdle`: **0x40 bytes**, zeroed, `{vptr, real CIdle*, ctx*, ...}` | `schedshim.cpp:KFakeIdleSize` | "zeroed, so inline reads of CActive fields see 'idle'" | **CODE** (works on the device for the games that use `CIdle`) | inline `IsActive()` reads see garbage |
| `CActiveScheduler` objects are 20 bytes (24 in Rodland) in the game | not a constant in the host (the host keeps only a side-table entry) | allocation size | **BIN** (information only) | - |
| Fake `CTrapCleanup`: `{vptr, 0}`, vtable word 2 = destructor, other slots a logging stub | `threadshim.cpp:gCleanupVt/gCleanupObj` | CODE | **CODE** | old code that uses the cleanup object beyond `Pop`/dtor would hit the logging stub |
| Fake audio stream: `iVptr` first, then host members. Vtable `CMdaAudioOutputStream`: **2 dtor, 3 SetAudioPropertiesL, 4 Open, 5 MaxVolume, 6 Volume, 7 SetVolume, 8 SetPriority, 9 WriteL, 10 Stop, 11 Position**; observer (no virtual dtor): **2 MaoscOpenComplete, 3 MaoscBufferCopied, 4 MaoscPlayComplete** | `audioshim.cpp` (header comment, `gAudVt`) | "verified from the S60v2 mdaaudiooutputstream.h" | **SDK**; **RUN** (Atomanic audio works on the device) | the game calls the wrong method, e.g. `Stop` instead of `Position` |
| Fake `TParse`: vptr 0, iWild 4, `iField[4]` 8..23, name buffer at **+24**, total **544** | `fsshim.cpp:KOldNameOff/KOldParseSize` (+ compile-time check `sizeof(TFileName) == 520`) | "verified from the v2 f32file.h" | **SDK**; **RUN** | the game's stack frame is overwritten (the object lives on the game's stack) |
| `TFakeDir {old vptr, CDir*}` (8 bytes) | `fsshim.cpp:TFakeDir` | host-defined facade; the old code only receives the pointer and calls the import shims `CDir::Count`/`operator[]` | **RUN** | - |
| `TEntry` has the same public layout in v2 and v3 (iAtt, iSize, iModified, iType, iName) | `fsshim.cpp` comment | SDK headers compared | **SDK** | directory listings with garbage names |
| Old `CArrayFixBase`: **24 bytes**; **+4 element count** (read inline by the game); +8 record length, +12 granularity, +16 data, +20 capacity are **host choices** | `memshim.cpp:ShimArrayCtor` | size and the count offset: the game's allocation and its inline reads (RedHood) | size and +4 **RUN** (RedHood plays); the other offsets **host-defined** (the old code only reaches them through imports) | an array whose count the game reads is wrong, e.g. a loop that never ends |

## 3. Vtable words used by the host

| word | meaning | where | status |
|---|---|---|---|
| application word **14** | `CreateDocumentL()` | `legacygame.cpp:CreateObjects` (`v[14]`) | **BIN** (the framework's own `CreateDocumentL(CApaProcess*)` is word 4) |
| document word **19** | `CreateAppUiL()` | `legacygame.cpp:CreateObjects` | **BIN** |
| AppUi word **15** | `ConstructL()` | `legacygame.cpp:RunConstructL` | **BIN** |
| AppUi word **19** | `HandleCommandL(TInt)` | `legacygame.cpp:HandleCommand` | **BIN**: overridden in some games, **Candy inherits it** (the word points at an import thunk `CEikAppUi::HandleCommandL`). `HandleCommand` does not check for that: with `softkey_*_cmd` set it would call the import. Off by default |
| AppUi word **8** | `CCoeAppUi::HandleKeyEventL(const TKeyEvent&, TEventCode)` | `ctlreg.h:KAppUiWordKey` | **BIN** (an import thunk in the games that do not override it; AnotherWorld overrides it) |
| AppUi word **9** | `CAknAppUi::HandleForegroundEventL(TBool)` | `ctlreg.h:KAppUiWordForeground` | **BIN** (AnotherWorld overrides it) |
| AppUi word **3** | `CAknAppUi::HandleWsEventL(const TWsEvent&, CCoeControl*)` | `legacygame.cpp:CallWsFocusEvent` | **BIN**: import at word 3; Candy overrides it. The code calls it with a 16-word zeroed `TWsEvent` whose first word is the type (10 focus lost, 11 focus gained): **CODE** |
| control word **3** | `OfferKeyEventL(const TKeyEvent&, TEventCode)` | `ctlreg.cpp:CtlReg_OfferKey` (`vt[3]`) | **BIN**: every `CCoeControl` subclass with a key handler overrides word 3 (AnotherWorld has none and keys go through the AppUi). The *name* of the word is **RUN** (Captain Dynamo's keys work), not read from the SDK header |
| observer words **2 / 3** | `MCoeForegroundObserver::HandleGainingForeground / HandleLosingForeground` | `ctlreg.cpp:CtlReg_CallForeground` | **BIN**: both are `__pure_virtual` in the base and overridden by the game; which of the two is gain and which is loss is **RUN** |
| "is it overridden?" test | an inherited slot points at an **import thunk**: `ldr rN,[pc,#4]; ldr rN,[rN]; bx rN; .word IATslot`, or the 12-byte `ldr rN,[pc]; ldr pc,[rN]` form | `ctlreg.cpp:CtlReg_IsImportThunk` (masks `0xE59F0004`, `0xE59F0000`), `legacygame.cpp:CallWsFocusEvent` (exact words `0xE59FC004, 0xE59CC000, 0xE12FFF1C`) | **BIN**: step0 finds exactly these thunks in the 8 games (16 bytes each). Thumb thunks are not handled (`CtlReg_IsImportThunk` answers "not a thunk" for an odd address) |
| minimum vtable sizes passed to `VTable()` (15, 24, 30, 4) | `legacygame.cpp` | **bounds checks only** ("the vtable must lie inside the image"), not layout claims. The real slot counts are 13 (application), 21 (document), 28 (AppUi) |

## 4. Fake graphics objects

| fact | where | source | status |
|---|---|---|---|
| every fake vtable has **128 words**; unused words answer 0 and log (first 2 hits per word, 16 in total) | `gfxfake.cpp:KFakeVtWords` | host choice, large enough for the biggest class used (the CBitmapContext vtable goes beyond word 49) | **CODE** |
| screen device: word **3** `DisplayMode()`, word **4** `SizeInPixels()` (hidden result pointer in r0, this in r1) | `gfxfake.cpp:GfxFake_Init`, `FakeSizeInPixels` | "verified in the binary" (comment) | **RUN** (all games draw) |
| graphics context: word **48** `BitBlt(const TPoint&, const CFbsBitmap*)`, word **49** `BitBlt(.., const TRect&)`; the point and the rect are passed **by reference** (r1, r3) | `gfxfake.cpp:ShimGcBitBlt/ShimGcBitBlt3` | "see the CBitmapContext vtable in e32/gdi.h" | **SDK**; **RUN** |
| graphics context: word **56** `DrawBitmap(const TRect&, const CFbsBitmap*)` (scaled draw) | `gfxfake.cpp` (gVtGc) | RedHood calls it | **RUN** (RedHood) |
| `CreateContext` on a bitmap device: the device object (`gVtDevice`: word 3 `DisplayMode`, word 4 `SizeInPixels`) has the bitmap in **word 1** of the object; the context (`gVtBmpGc`) copies it to its own **word 1**, and drawing goes into that bitmap. A context for any other device is `gVtZero` (every word returns 0, first calls logged) | `gfxfake.cpp:ShimCreateContext` | host-defined facade | **RUN** (RedHood) |
| `CreateBitmapL` / `.mbm` loading: real `CFbsBitmap::Load(path, id)` + `GetScanLine(row, TPoint(0, y), width, EColor4K)` into the fake bitmap, which gets **mode 10**. Needs an `RFbsSession` in every game thread that loads | `gfxfake.cpp:LoadIntoFake`, `EnsureFbsSession` | the phone's own decoder | **RUN** (RedHood, Crystal) |
| a frame reaches the screen only if the **fake screen mode == 7** and the bitmap mode is **7 or 10** (10 is converted with `Convert4KTo565`) | `gfxfake.cpp:BlitToScreen` | host rule | **RUN**. The profile key `display_mode` sets the screen mode |
| fake `CFbsBitmap` (also what `CWsBitmap` creates): **0x18 bytes**; **+8 pixels, +0xc width, +0x10 height, +0x14 display mode**; vtable `gVtBitmap`, word 2 = destructor | `gfxfake.cpp:ShimBitmapCtor/ShimBitmapCreate` | the size is the game's allocation | size **BIN**; the field positions are **host-defined** (the old code only uses the object through imports) |
| old code may write up to **4096 bytes past the bitmap** | `gfxfake.cpp:KBitmapSlack` | found with MagicPuz ("~1 row"), comment | **RUN** |
| rows are **4-byte aligned**: stride `(w * bytesPerPixel + 3) & ~3` | `gfxfake.cpp:ShimBitmapCreate`, `BlitToScreen` | old bitmap server behaviour | **RUN** (mode 7 and 10 games); other modes are not copied |
| display modes: **7 = 64K (2 bytes), 10 = 4K, 8 = 16M (3), 9 and 11 = 4 bytes**; only 7 reaches the screen, 10 is converted | `gfxfake.cpp:BytesPerPixel`, `BlitToScreen` | `TDisplayMode` values | **SDK**; 7 and 10 **RUN** |
| screen memory: **0x20-byte palette header**, then the pixels (RGB565) | `gfxfake.h:KFakeFbHeader` | "direct screen access, +0x20 palette header" | **RUN** |
| `SScreenInfoV01` as `{TInt, TAny*, TInt, TAny*, TInt, TInt}` = window handle valid/handle, screen address valid/address, width, height | `gfxfake.cpp:TOldScreenInfo`, `ShimScreenInfo` | CODE | **RUN** |
| fake `CDirectScreenAccess`: **0x40 bytes**; **+0x18 `iGc`, +0x1c `iScreenDevice`, +0x20 `iDrawingRegion`** | `gfxfake.cpp:ShimDsaNewL` | CODE | **RUN** |
| old `TRawEvent`: type in word 0; **type 5 = redraw** (a frame was copied) | `gfxfake.cpp:ShimRawEventSet/ShimAddEvent` | CODE | **RUN** (frame counting works) |

## 5. Facts that belong to one game (profile keys, not constants)

| fact | where | default | status |
|---|---|---|---|
| `Dll::Tls` data at byte **0x60** holds a pointer to the game's control block (`stop_state_tls`); the state word at **+0x20** of that block (`stop_state_field`) set to **3** (`stop_state_value`) makes the game's signal handler stop its scheduler | `profile.cpp` (defaults `0x60 / 0x20 / 3`), `threadshim.cpp:StopBlock`, `ThreadShim_StopAndWait` | the **Captain Dynamo** layout | **GAME**. `profile.h` says: any game with another layout must switch it off with `stop_state_tls=0` |
| screen size, display mode | profile keys `screen_width/screen_height/display_mode` (`profile.cpp`), `gfxfake.cpp` defaults 176x208 mode 7 | the S60v2 screen; the `display_mode` the tools write into a new profile is only a **guess**: it must be the screen mode the game draws for (RedHood: 7), see cookbook, mistake 25 | **GAME** |
| minimum thread stack **0x10000** | `threadshim.cpp:gMinStack`, key `thread_stack_min` (0x4000..0x40000) | old frames up to ~1.8 KB plus the 550-byte `TParse` | **GAME** |
| key scan codes of the Captain Dynamo handler (`0x0e` left ... `0xa5` right softkey) | comment in `legacygame.cpp:OfferKey` | - | information only, the host passes scan codes unchanged |
| key plain | profile key (only used for Snails game) | 0 as default (1 = the control-stack route also gets the plain EEventKey) | **GAME**; Snails |
| auto_cmd (command id sent once to the game's AppUi::HandleCommandL after start-up) | profile key (only used for Snails game) | 0=none as default, 1 for Snails | **GAME**; Snails |

**The stop-state trick is the one to look at first on a second game.** It reads and **writes** through a pointer that comes out of the game's own memory. A game with a different layout must have `stop_state_tls=0` in its profile; the host logs `exit trick applied (tls .., block ..): state a -> b` or `exit trick not applied` so the log shows which happened.

## 6. Calling-convention and data facts (old ABI)

| fact | where | status |
|---|---|---|
| `TPtr8`/`TPtr16`/`TPtrC`: (length, max length, pointer), no vptr, **the same in v2 and v3**; `TPtrC` is 8 bytes and is returned in r0:r1 | `compatshim.cpp` header comments, `fsshim.cpp` compile-time `sizeof(TPtrC) == 8` | **SDK**, **RUN** |
| `TFileName` = `TBuf<256>` = **520 bytes** in both SDKs | `fsshim.cpp:TCheckFileName` (compile-time check) | **SDK** (checked by the compiler) |
| a `TInt64`/`TTime` inside an old object may be only **4-byte aligned** | `compatshim.cpp:ShimHomeTime`, `Ld64`/`St64` | **CODE** (access as two 32-bit words, see the cookbook, pattern 6) |
| `TTime::MicroSecondsFrom`: hidden result pointer r0, this r1, the by-value `TTime` in **r2:r3** | `compatshim.cpp:ShimMicroSecondsFrom` | "verified in the binary" (comment) |
| doubles: **r0 = high word, r1 = low word**; a double result comes back as (lo << 32) \| hi | `rtshim.cpp` | **RUN** (math shims used by games) |
| `TKeyEvent` = (iCode, iScanCode, iModifiers, iRepeats), the same in v2 and v3; the old code reads `iScanCode` at **+4** | `legacygame.cpp` comment, `ctlreg.cpp` | **SDK**; **RUN** |
| `EKeyWasConsumed` = **1** | `ctlreg.cpp:CtlReg_OfferKey`, `CtlReg_AppUiKey` | **RUN** |
| destructors are called `dtor(this, flags)`, bit 0 = free the memory | cookbook pattern 7 | **RUN** (flags 1 and 3 in the logs) |
| `TCallBack` by value = two registers (r1:r2) | cookbook pattern 5 | **RUN** |
| TMdaAudioDataSettings rate/channel constants: the same values in v2 and v3 (e.g. 8000 Hz = 0x10 ... 96000 Hz = 0x20000; mono 0x02000000, stereo 0x04000000) | `audioshim.cpp:CapsToRate/RateToCaps`, `VSetAudioPropertiesL` | **CODE** ("the same values in v2 and v3"); **RUN** (Atomanic) |
| old `TThreadPriority` values -30..30 and 100..500 are renumbered in v3 | `threadshim.cpp:ShimThreadSetPriority` | **CODE**; **RUN** |
| old heap memory is **zero** when the game gets it (old code never initialises some fields) | `memshim.cpp:ShimAllocL`, `ShimBuiltinNew`, `ShimCBaseNewL` use `User::AllocZ` | **RUN** (Qwak on Belle died without it; S60 3rd happened to work) |
| old exceptions are signals: `RThread::SetExceptionHandler` / `RaiseException` | `threadshim.cpp` (a real active object in the game thread delivers the signal); game threads are created with **no own heap** (they share the creator's) | **RUN** (Qwak) |

## 7. The old image format (E32Image, pre-Symbian-9)

| fact | where | status |
|---|---|---|
| header: UID1..3 at 0x00/0x04/0x08, signature `'EPOC'` (0x434F5045) at **0x10**, compression field **0x1C** (non-zero = not supported), flags 0x2C, code size 0x30, data size 0x34, heap min/max 0x38/0x3C, stack 0x40, bss 0x44, entry 0x48, code base 0x4C, DLL ref count 0x54, export dir offset/count 0x58/0x5C, text size 0x60, code offset 0x64, import offset 0x6C, relocation offsets 0x70/0x74; minimum header 0x7C | `legacyimage.cpp` (parser), `gamelocator.cpp` (UID2 0x100039CE, signature) | **BIN**: step0 checks the UID checksum, section bounds and the import section on every game; the parser loads all of them |
| UID1 `0x10000079` = DLL, `0x1000007A` = EXE; the games are application DLLs (UID2 `0x100039CE`) | `legacyimage.cpp`, `gamelocator.cpp` | **BIN** |
| export ordinal **1** = `NewApplication` | `legacygame.cpp:CreateObjects` (`ExportEntry(0)`) | **BIN** (step0 lists export 1 for each game) |
| import section: size word, then per DLL (name offset, count, ordinals...) ; at most 64 DLLs, 8192 imports per DLL | `legacyimage.cpp` | limits are host choices; the format is **BIN** |
| the IAT follows the text section; its words equal the import ordinals in the file | `step0.py` check "IAT found and its words equal the import ordinals" | **BIN** |
| writable static data (data size or bss size != 0) is refused | `legacyimage.cpp` | host limit; none of the games has any |
| an import thunk is **16 bytes** (ARM `ldr; ldr; bx; .word`) | step0 `thunk_sizes` = {16} | **BIN** |

## 8. Host-side constants that look like layout facts but are not

| constant | where | why it is there |
|---|---|---|
| trap/trace stub = **13 words / 52 bytes** | `iatfill.h:KIatStubSize`, `iatfill.cpp:MakeStub` | the host's own code, see the cookbook 1.1 |
| call helpers (`KSetJmpCode`, `KLongJmpCode`, `KCall1Code`, `KCallACode`, `KTrapCode`) | `oldrt.cpp` | hand-assembled ARM, 4 registers pushed so that `sp` stays 8-aligned for the callee |
| `KMaxOldThreads` 8, `KMaxActive` 16, `KMaxSched` 8, `KMaxDerivedIdle` 8, `KMaxParse` 128, `KMaxBufs` 8, `KMaxSlots` 8, `KSigQueue` 8, `KMax` 16 (ctlreg) | the files named | table sizes, not layouts. A full table logs `FAILED` or is ignored; size them for the games in `games\` |
| `KIatStubHitReason` -9999, `KHostExitLeave` -9001 | `iatfill.h`, `lhgui.cpp` | old leave codes the host invents; they must not collide with Symbian error codes |

## 9. Checking a new game

1. `python tools\layout_check.py <game.app> --map games\<id>\map.csv` (a new game: the `map.csv` that `triage.py` / `newgame.py` write; the tool runs step0 for you; see `docs\tools\layout_check.md`) prints, for the game:
   - the **allocation sizes** of the framework classes (`CEikApplication`, `CEikDocument`/`CAknDocument`, `CEikAppUi`, `CFbsBitmap`, `CActiveScheduler`) and whether they equal the facts above;
   - the **vtable slots** the game overrides for the application, document, AppUi, control and active-object classes, against words 14 / 19 / 15, 19, 8, 9, 3 / 3 / 3-4-5;
   - whether the game reads **`CCoeEnv` +0x3c / +0x18** after `CCoeEnv::Static()`;
   - the **stop-state** warning: the profile default is the Captain Dynamo layout.
   Exit 0 = all as in the tables, 1 = a difference (listed), 2 = cannot run. On the 8 games in `games\` it reports no difference.
2. A difference is **not an error by itself**: a game may simply override less. It is a reason to read the row above before the first device run.
3. After the first run, look in the log for `exit trick applied` / `not applied`, `fake vtable word n called` (gfxfake), `UNEXPECTED virtual call on facade TParse` (fsshim) and `FAILED` (full tables). These are the host's own alarms for a wrong layout.

## 10. Things nobody has checked yet

- `CEikApplication` `iProcess` at +20 and the field names of `CEikDocument` (CODE only).
- The size and fields of the old `CActive` (CODE only; consistent with the slots, which are verified).
- `gFakeEnv` is zero except +0x3c: any game that reads another `CCoeEnv` field and **uses** it (not just passes it on) is unknown territory. 7 games read +0x18 and pass it to `CEikAppUi::Application()`, which the host ignores.
- `HandleCommandL` on a game that does not override it (Candy) with `softkey_*_cmd` set.
- Thumb-state games (`CtlReg_IsImportThunk` and the exact-word test in `CallWsFocusEvent` know only ARM thunks). All supported games are ARM (step0 `mode: arm`, 16-byte thunks).
- Bitmap **masks** and alpha: not implemented (`ShimBitmapLoad`, fbscli 76, still not impemented compleletely; loading goes through the
  device decoder only for `CreateBitmapL`).
- `RSystemAgent::GetState` (Rodland): not implemented.
- Which `CArrayFixBase` offsets other games read inline (only +4 and the size 24 are known, from RedHood).
- Games with an Eikon menu in their `.rsc` (RedHood: commands 1, 2, 3, 0xBC1): the host sends one command per softkey and
  shows no menu.