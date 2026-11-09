# Shim cookbook

How to write a shim for SymBridge by copying a pattern that already exists in the code, instead of rediscovering it.

A **shim** is a host function that stands in for one import of an old (S60 v1/v2, EKA1, GCC 2.95) game binary. The game
calls it through its import table with the **old** calling convention and the **old** object layouts; the shim answers using the
real S60 v3 API. Everything here was taken from the sources in `src\`; every `file.cpp:Name` reference is a real function, class
or table and is checked by `tools\tests\test_cookbook_refs.py`.

Read chapter 1 first (the rules that apply to every shim), then pick your pattern in chapter 2.

| chapter | content |
|---|---|
| 1 | What a shim is, how it is called and registered, and the rules for all shims |
| 2 | Which pattern do I need? (decision table) |
| 3 | The patterns, each with *when to use*, *skeleton*, *real example*, *usual mistakes* |
| 4 | The mistakes list (all of them in one place) |
| 5 | Finding out what the game really does: the workflow from a trap in the log to a finished shim |
| 6 | Checklist before you commit |

---

## 1. Ground rules

### 1.1 What happens when the game calls an import

At load time `TIatLinker::Link` (`iatfill.cpp:TIatLinker::Link`) fills the game's import table, one slot per import
(dll, old ordinal). For each slot the **shim registry** decides, with one lookup (`shimreg.cpp:ShimReg_Resolve`):

| registry says | slot gets | log counter |
|---|---|---|
| a function address | your shim | `shim=` |
| the DIRECT flag, and the import map has an exact v3 ordinal | the real v3 function, straight from the DLL | `direct=` |
| nothing | a trap stub (`iatfill.cpp:MakeStub`) | `trap=` |

Your shim is therefore called **by old code, on the old code's stack, with the old code's registers**. There is no wrapper in
between. That one sentence explains most of the rules below.

An unresolved import (trap stub) does not stop the game: in trace mode it logs `TRACE #n dll ordinal args=...`, returns 0 (or
`this` for constructor-like imports) and the game goes on (`iatfill.cpp:TrapHandler`). The first hits of each import are listed at
the end of the run as `hits #n dll ordinal = count`. That list is how you find out which shim to write next.

### 1.2 Calling convention: what is the same, what is not

For simple signatures (integers, pointers) the old GCC 2.95 convention is **identical to EABI**: arguments in r0-r3 then the
stack, result in r0. That is why a plain C function with the same parameters can be used as the shim
(`memshim.cpp:ShimMemCopy`). Member functions get `this` in r0 as the first argument; static functions have no `this`.

The differences you must know about:

| case | old code does | what the shim must do | example |
|---|---|---|---|
| **double** (`TReal64`, `double`) | passes it as two words, **r0 = high word, r1 = low word** (FPA order) | take two `TUint32` per double (hi, lo); return a `TUint64` built as `(lo << 32) \| hi` | `rtshim.cpp:ShimMuldf3` |
| **64-bit integer by value, after other arguments** | packs it right after the previous argument, **no even-register alignment** | declare it as two `TUint32` (lo, hi). EABI would skip a register | `compatshim.cpp:ShimMicroSecondsFrom` |
| **64-bit integer at the start** (`__divdi3`) | low word in the lower register | same as EABI: a plain `TInt64` parameter works | `rtshim.cpp:ShimDivdi3` |
| **TInt64 / TTime inside an old object** | the object may be only **4-byte aligned** | never dereference a `TInt64*`; read and write two 32-bit words | `compatshim.cpp:Ld64`, `compatshim.cpp:St64` |
| **result larger than 8 bytes** (`TRect`, `TFileName`) | caller passes a **hidden result pointer in r0**; `this` moves to r1 | a C++ function returning the struct by value gets the same layout from GCCE, or take the pointer explicitly | `lhgui.cpp:ShimClientRect`, `fsshim.cpp:ShimAppFullName` |
| **result of 8 bytes** (`TPtrC`) | can come back in **r0:r1** | return a `TUint64` (r0 = low word) | `fsshim.cpp:ShimParseDriveAndPath` |
| **class passed by value** (`TCallBack`, 8 bytes) | its two words arrive in two consecutive registers | declare two parameters | `schedshim.cpp:ShimIdleStart` |
| **more than 4 argument words** | the rest on the stack, in order | list them all as parameters, as in the old signature | `threadshim.cpp:ShimThreadCreate` |
| **constructor** | returns `this` in r0 | `return aThis;` | pattern 2 |
| **destructor** | called as `dtor(this, flags)`; bit 0 of `flags` = also free the memory | take the second parameter and honour bit 0 | pattern 7 |

Do not trust these tables for a new signature. **Verify the convention in the disassembly of the real call sites** (chapter 5).
The existing shims say "verified at N call sites" in their comments for exactly this reason; `ShimParseDriveAndPath` and
`ShimMicroSecondsFrom` both look like they need a hidden pointer and one does not.

### 1.3 Old objects

Old objects are laid out by the **old** compiler and **old** headers:
- virtual calls go through a **GCC 2.95 vtable**: two header words, then the virtual functions from word 2 (a class with a virtual
  destructor has it in word 2). The v3 vtable is different: never give old code a real v3 object that it will call virtually
  (pattern 4), and never call a v3 destructor on an old object (`memshim.cpp:OldDeleteCBase`).
- old inline code reads **fields directly** at old offsets (`CActive::iStatus` at +4, `CCoeControl::iCoeEnv` at +8). A facade
  must have those bytes in place and zeroed (`activeshim.cpp:KOldActiveSize`, `schedshim.cpp:KFakeIdleSize`).
- memory of a game object comes from `CBase::operator new`, which is **zeroed** (`memshim.cpp:ShimCBaseNew`). Base-class
  constructors therefore often have nothing to do but `return this` (`eikshim.cpp:ShimNoopCtor`).
- the pointer you receive may be a **subobject pointer** (multiple inheritance). The audio observer is an
  `MMdaAudioOutputStreamCallback*`, not the game's object: use *its* vptr and *its* slot numbers (`audioshim.cpp:Deliver`).

### 1.4 Leaving, and entering old code

Old code has **no unwind information**. Two consequences, and they are the two most important rules of the project:

1. **Never leave or throw the v3 way through old frames.** `User::Leave`, a v3 function that leaves, a C++ exception: all of
   them travel up the stack as a C++ exception and cannot pass old frames. Use `TOldRt::Leave(reason)` (old-style longjmp to the
   innermost old trap frame) and call v3 functions that can leave inside `TRAP`. See pattern 8.
2. **Never call old code directly.** Enter it only through `TOldRt::Call0 / Call1 / CallArgs`, which set up an old-style trap, so
   that an old leave comes back to you as a return value. See pattern 5.

All the old-runtime state (trap chain, cleanup depth, TLS value, "is this the game thread") is **per thread**
(`oldrt.h:TOldThreadState`). A thread that runs old code must be attached (`TOldRt::AttachThread`); `TOldRt::State()` panics
`OLDRT 2` for a thread that is not.

### 1.5 Registering a shim

A shim is a row in a static table (`shimreg.h:TShimRegEntry`): lower-case DLL name without `.dll`, old ordinal, address, flags,
owner name, resolver:

```cpp
static const TShimRegEntry KMyShimEntries[] =
    {
    { "euser", 1234, (TUint32)&ShimThing, 0, "myshim", NULL },   // TFoo::Thing(int)   <- always name the function
    { "euser", 1235, (TUint32)&ShimOther, 0, "myshim", NULL }    // last row: no trailing comma
    };

TInt MyShim_Init()
    {
    return ShimReg_Add(KMyShimEntries, sizeof(KMyShimEntries) / sizeof(KMyShimEntries[0]));
    }
```

Real example: `compatshim.cpp:KCompatShimEntries`, `compatshim.cpp:CompatShim_Init`.

- A **new table** (new file) must be added to `legacygame.cpp:KRegSteps` and its `MyShim_Init` declared in a header.
  (`rtshim.cpp:RtShim_Init` and `lhgui.cpp:HostShim_Init` are called from `lhgui.cpp` instead; both ways work. New code should use
  `KRegSteps`.) A new `.cpp` also goes into the `SOURCE` list of the project's `.mmp`.
- A **new row in an existing table** needs nothing else.
- One (dll, ordinal) may be claimed **once** in the whole registry. A second claim (also an old DIRECT row in `directbind.cpp`) makes
  the linker refuse to link: `shimreg.cpp:ShimReg_CheckDuplicates` logs `DUPLICATE dll n: ownerA and ownerB`. An entry with no
  address, no resolver and no DIRECT flag is logged as `EMPTY entry`.
- **No static constructors.** Tables are plain `static const` data added explicitly at start-up; objects that need setup get an
  `XxxShim_Init(THostLog*)` that the host calls (`lhgui.cpp:CLhAppUi`, `threadshim.h:ThreadShim_Init`).
- Comment every row with the **old C++ name** (`// TTime::HomeTime()`): the ordinal alone is unreadable six months later. The name
  comes from `games\<id>\map.csv` or the triage report.
- The ordinal is the **v2 SDK ordinal**, not the v3 one. A shim does not need the import map at all.
- A runtime-resolved entry (`EShimRegResolvedAtRuntime`) is for an address that only exists after start-up, such as a code stub in
  a chunk: `oldrt.cpp:KOldRtShimEntries`.

### 1.6 Rules for every shim (short form)

1. Return in the old convention (chapter 1.2). Constructors return `this`.
2. No v3 leave, throw or `User::Leave` through old frames: `TOldRt::Leave`.
3. Old code only through `TOldRt::Call*`.
4. State that belongs to a thread is per thread; shared tables are locked and remember the owning thread.
5. Treat every pointer from old code as untrusted: check `NULL`, alignment, and that a vtable word is non-zero.
6. A shim runs on the **old thread's stack**: keep locals small (a `TFileName` is 520 bytes; the thread minimum is 0x10000, see
   `threadshim.cpp:gMinStack`) and do not recurse.
7. Log what is interesting **a few times** (`if (gLog && gXxxLogs < N)`), never every call: a game frame loop calls shims thousands
   of times per second.
8. Never block the UI thread for ever (`threadshim.cpp:ShimSemWait` bounds waits on a non-game thread).

---

## 2. Which pattern do I need?

| the import is ... | pattern |
|---|---|
| a function that takes and returns integers and pointers and has an equivalent in v3 (or is easy to write) | 1 plain function |
| a constructor of a small value class (descriptor, rect, int64, ...) | 2 constructor |
| a constructor or method of an object the old code allocates, whose **state must live in the old object** (TParse) | 3 facade with old layout |
| a function that **creates an object the game calls virtually** (`NewL` returning a class with virtuals) | 4 object with an old-style vtable |
| the v3 side has to **call the game back** (callback, `RunL`, thread function, idle function) | 5 calling back into old code |
| a `double`, `float` or 64-bit integer is passed, returned or stored | 6 doubles and 64-bit integers |
| the base-class constructor or destructor of a class **the game derives from** | 7 destructors and base classes, 9 paired objects |
| the v3 function can **leave** (`...L` functions, `User::Leave*`) | 8 leaving |
| the game derives from `CActive`, `CIdle`, `CActiveScheduler` | 9 paired real object in a side table |
| the answer needs **host objects** (the real AppUi, the screen, the exit request) | 10 host-object shim |
| an `RFile`/`RSemaphore`/`RMutex`-like handle where only one detail differs (a path, a wait) | 11 handle wrapper |
| thread creation, `Dll::Tls`, the cleanup stack of a new thread | 12 per-thread state |
| an exact v3 match exists and the function is plain | 13 direct bind (no code) |
| the address exists only at run time | 14 runtime-resolved entry |
| a request completed from another thread (`RThread::RequestComplete`) | 15 cross-thread completion |

If two patterns apply, combine them: `ShimAudioNewL` is 4 + 5 + 8; `ShimIdleCtor` is 2 + 7 + 9.

---

## 3. The patterns

### Pattern 1 - Plain function

**When to use.** The old call is an ordinary function or member function with integer/pointer arguments and an integer/pointer
result, and v3 has the same behaviour (or a few lines reproduce it). The old compiler and EABI agree on such signatures, so the
shim is a normal C function whose parameter list is the *old* signature with every reference turned into a pointer.

**Skeleton.**
```cpp
// euser 1234: TInt TFoo::Thing(TInt aA, const TDesC& aName) const
static TInt ShimFooThing(const TFoo* aThis, TInt aA, const TDesC* aName)   // this first; a reference is a pointer
    {
    return aThis->Thing(aA, *aName);          // forward to the real v3 implementation
    }
```
Add a row `{ "euser", 1234, (TUint32)&ShimFooThing, 0, "myshim", NULL }` (chapter 1.5).

**Real examples.**
- Forward unchanged: `memshim.cpp:ShimMemCopy`. Mind the old argument order: `Mem::Fill(aTrg, aLen, aChar)` - `memshim.cpp:ShimMemFill`.
- The v3 function has an extra parameter: `compatshim.cpp:ShimReAlloc` (the map row is "name only", so it can never be a direct bind).
- v3 has no export (inline in the SDK): write it: `compatshim.cpp:ShimMemMove`, `compatshim.cpp:ShimMemCompare`.
- Replace a compiler helper: `compatshim.cpp:ShimDivsi3`, `rtshim.cpp:ShimModsi3` (a zero divisor returns 0 instead of trapping; a decision,
  keep the game running, written down in the comment).
- Answer a question the phone would have answered: `compatshim.cpp:ShimGetMachineIdL` returns a fixed, well-known example IMEI.

**Usual mistakes.**
- Thinking of a reference parameter (`const TDesC&`) as a value: the old caller passes a **pointer** (a reference has the same
  ABI). Write the pointer in the shim: it shows the ABI and lets you check for `NULL`.
- Forgetting that a `TBool`/`TInt` result is read from the whole of r0: return 0 or 1, not a truncated value
  (`compatshim.cpp:ShimTInt64Less` returns the comparison as `TInt`).
- A function that returns `void` in v3 but returned a pointer in the old SDK: the caller may use r0 (`memshim.cpp:ShimMemFill`
  returns `aTrg`).
- Writing a shim for something the v3 SDK would let you **bind directly** (pattern 13). Prefer direct only when the rules in the
  header of `directbind.cpp` allow it; a shim is allowed to be more careful.
- A v3 function with an **extra parameter or a different default** (`User::ReAlloc` has a `mode`): pass the value you want
  explicitly, as `compatshim.cpp:ShimReAlloc` does.

### Pattern 2 - Constructor (return `this`)

**When to use.** The old code calls a constructor on memory it already owns (stack, member, or `new`). The old GCC 2.95 convention
is: `this` in r0, **the constructor returns `this` in r0**, and the memory is **zeroed** if it came from `CBase::operator new`.

**Skeleton.**
```cpp
// euser 1234: TFoo::TFoo(TInt aA)
static TAny* ShimFooCtor(TAny* aThis, TInt aA)
    {
    new (aThis) TFoo(aA);        // ONLY if v2 and v3 TFoo have the same size and layout and there is no vptr
    return aThis;                // <- not optional
    }
```
Variants, in order of preference:
1. **Same layout, no vptr**: placement new of the real v3 class. `compatshim.cpp:ShimTPtr8Ctor`, `compatshim.cpp:ShimTPtrC16Ctor`,
   `compatshim.cpp:ShimTRectCtor`. A protected v3 constructor: build a trivial subclass and placement-new that
   (`compatshim.cpp:TBufBase16Access`, `compatshim.cpp:ShimTBufBase16Ctor`).
2. **Write only the fields the old inline code reads**: `lhgui.cpp:ShimCoeControlCtor` sets `iCoeEnv` at +8 and nothing else.
3. **Copy fields from the arguments**: `eikshim.cpp:ShimAknDocCtor` (document: application at +8, process at +12; offsets are in the
   comment, "verified against the binary").
4. **Nothing to do** (zeroed memory): `memshim.cpp:ShimCBaseCtor`, `eikshim.cpp:ShimNoopCtor`, `eikshim.cpp:ShimAppUiCtor`.
5. **Plain data** `TInt64`: store as two words, `compatshim.cpp:ShimTInt64Ctor`.

**Usual mistakes.**
- **Not returning `this`.** The old caller uses r0 as the object (`new` expressions chain on it). The symptom is a crash a few
  instructions later, far from the shim. The trap stub returns its first argument for constructor-like imports for the same
  reason (`iatfill.cpp:TrapHandler`).
- Placement-new of a v3 class with a **different size**: it writes past the old object. Add a compile-time check, as
  `fsshim.cpp` does: `typedef char TCheckFileName[(sizeof(TFileName) == 520) ? 1 : -1];`.
- Placement-new of a class **with a vptr** over an old object: word 0 of an old object is the old vptr. A base-class constructor
  is fine (the game's own constructor sets its vptr afterwards, `schedshim.cpp:ShimSchedCtor` says so), a final object is not.
- Assuming a class has the same layout in v2 and v3. Descriptors (`TPtr`, `TBuf`, `TPtrC`) and `TRect` do (the comments in
  `compatshim.cpp` say so); check every other class against the v2 and v3 headers and the binary.
- Not noticing which **variant** the old binary imports (C1/C2, complete/base object). The map column `v3_variant` records it;
  the shim is one function for the ordinal the game actually uses.
- Initialising more than the old constructor did: the old object is smaller than the v3 one.

### Pattern 3 - Facade object with the old layout

**When to use.** Old code **allocates the object itself** (usually on its stack) and then calls several functions on it
(`TParse`). The v3 class has a different size or layout, or a v3 vptr that old code must not see. So the old object keeps the
**state in the old layout**, and each shim call rebuilds a real v3 object from it, does the work, and stores the result back
("stateless facade"). If old code deletes it through a vtable, give it an old-style vtable (pattern 4).

**Skeleton.**
```cpp
const TInt KOldThingSize = 544;          // size of the OLD class, from the v2 header
const TInt KOldNameOff   = 24;           // offset of the member the state lives in
static TUint32 gVtThing[8];              // old-style vtable; filled in XxxShim_Init (slots -> logging stub)

static TAny* ShimThingCtor(TUint8* aThis)
    {
    Mem::FillZ(aThis, KOldThingSize);
    *(TUint32*)aThis = (TUint32)gVtThing;                  // old vptr, word 0
    new (aThis + KOldNameOff) TFileName;                   // member with the same layout in v2 and v3
    RegisterThing((TUint32)aThis);                         // remember facade addresses (see mistakes)
    return aThis;
    }

static TInt ShimThingSet(TUint8* aThis, const TDesC* aName)
    {
    TParse real;                                           // real v3 object, rebuilt on every call
    const TInt r = real.Set(*aName, NULL, NULL);
    if (r == KErrNone)
        {
        ((TFileName*)(aThis + KOldNameOff))->Copy(real.FullName());      // store the result in the OLD object
        }
    return r;
    }
```

**Real examples.** `fsshim.cpp:ShimParseCtor`, `fsshim.cpp:ShimParseSet`, `fsshim.cpp:ShimParseAddDir`, `fsshim.cpp:ShimParsePopDir`;
size and offsets `fsshim.cpp:KOldNameOff`, `fsshim.cpp:KOldParseSize`; the address registry `fsshim.cpp:RegisterParse`,
`fsshim.cpp:IsParse`; the vtable whose unexpected slots log `fsshim.cpp:ParseVirtualStub`.

**The host owns the class inside the game's allocation** (`memshim.cpp:ShimArrayCtor`, the old `CArrayFixBase`, RedHood). The
    game allocates the 24-byte object itself and reads the element count inline, so the host keeps its own state in the same
    24 bytes: **count at +4 and the size 24 are verified** (read by the game), everything else is the host's choice: +8 record
    length, +12 granularity, +16 data pointer, +20 capacity. Record such a split in `LAYOUT_FACTS.md` (what the game reads vs.
    what the host chose), so nobody mistakes a host choice for a fact about old code.
	
A second form is the **wrapper facade** `{ old vptr, real pointer }` for a v3 object the old code only passes around and
deletes: `fsshim.cpp:TFakeDir`, `fsshim.cpp:WrapDir`, `fsshim.cpp:DirFakeDtor`; the shims unwrap it
(`fsshim.cpp:ShimCDirCount`, `fsshim.cpp:ShimCDirIndex`). The entries it hands out (`TEntry`) have the same layout on both sides, so they are returned as they are.

**Usual mistakes.**
- **Returning a pointer into a temporary.** `TParse::DriveAndPath()` points into the temporary v3 `TParse`; the shim must return
  a pointer into the **old object's own buffer** (`fsshim.cpp:DriveAndPathOf` shows how: compute the offset, re-base it).
- Using an address **not created by your constructor**: old code sometimes calls a method on garbage. Keep a small ring of
  facade addresses and refuse unknown ones (`fsshim.cpp:IsParse`, returns `KErrArgument` or an empty result).
- A size that is *almost* right: check the old header (`sizeof` of the old class, verified from the binary's `new(N)` size, the
  triage page lists it under "Object sizes").
- Leaving an unused vtable slot at 0: an unexpected virtual call then jumps to address 0. Fill all slots with a stub that logs
  once and returns 0.
- Forgetting that `TFileName` is 520 bytes (v3 `TBuf<256>`): do not keep several on the old thread's stack.

### Pattern 4 - Object with an old-style vtable

**When to use.** An import **creates an object that the game then uses through virtual functions** (`CMdaAudioOutputStream::NewL`,
`CIdle::New`). The object must look like an old object: word 0 is a pointer to a **GCC 2.95 vtable**. A real v3 object has a v3
vtable (different layout, two destructor entries) that the old code would misread. So the old code gets a **facade**; the real
v3 object, if any, lives behind it.

**GCC 2.95 vtable layout.** Two header words (zero is fine), then the virtual functions **in declaration order, base class
first**, starting at word 2. A class with a virtual destructor has it at word 2, called as `dtor(this, flags)`. Slot numbers come from the
**v2 header** of the class (count them; the comment above `audioshim.cpp:gAudVt` shows how to write them down).

**Skeleton.**
```cpp
static TUint32 gThingVt[16];                       // [0],[1] header   [2] dtor   [3].. virtuals in v2 header order

struct TFakeThing
    {
    TUint32 iVptr;                                 // MUST be the first word: all the old code looks at
    TAny*   iObserver;                             // old callback object (maybe a SUBOBJECT pointer)
    TUint64 iThread;                               // creating thread: the only one allowed to use it
    // ... your state
    };

static void VDtor(TFakeThing* s, TInt aFlags)      // [2]
    {
    // stop timers, delete the real object, clear the observer so no callback reaches old code any more
    if (aFlags & 1) { User::Free(s); }
    }
static void VDoIt(TFakeThing* s, TInt aA) { /* [3] this = old object, arguments as in the v2 header */ }

void ThingShim_Init(THostLog* aLog)                // called once by the host, before the game runs
    {
    gThingVt[2] = (TUint32)&VDtor;
    gThingVt[3] = (TUint32)&VDoIt;
    }

static TAny* ShimThingNewL(TAny* aObserver, TAny* /*aServer*/)      // the import that creates the object
    {
    TFakeThing* s = (TFakeThing*)User::AllocZ(sizeof(TFakeThing));
    if (!s) { TOldRt::Leave(KErrNoMemory); }       // NewL leaves: pattern 8
    s->iVptr = (TUint32)gThingVt;
    s->iObserver = aObserver;
    s->iThread = MyId();
    return s;
    }
```

**Real examples.**
- Full: `audioshim.cpp:TFakeStream` (state), `audioshim.cpp:gAudVt` (slots), `audioshim.cpp:VWriteL`, `audioshim.cpp:VStop`,
  `audioshim.cpp:VDtor`, `audioshim.cpp:ShimAudioNewL`, `audioshim.cpp:AudioShim_Init` (fills the table). The facade does the playing with a real
  v3 stream (`audioshim.cpp:CRealAudio`) and **falls back to a silent, correctly paced fake** when that fails
  (`audioshim.cpp:GoFake`): a game that gets no sound must still run.
- Small: `schedshim.cpp:ShimIdleNew`, `schedshim.cpp:gIdleVt`, `schedshim.cpp:IdleFakeDtor`; `threadshim.cpp:ShimCTrapCleanupNew`
  (a fake with a no-op destructor, because the real cleanup stack already exists).
- Unexpected virtual calls are logged, not crashed: `schedshim.cpp:FakeSlot`.
- **A vtable with many words you do not know yet** (the fake graphics context, `gfxfake.cpp:gVtGc`, `gVtBmpGc`, `gVtZero`): fill
  the words you know, point every other word at a stub that returns 0 **and logs the first calls** (`gfxfake.cpp:WordHit`), so a
  missing word shows up in the log as "fake vtable word n called" instead of as a blank screen. Words filled so far:
  48 `BitBlt(const TPoint&, const CFbsBitmap*)`, 49 `BitBlt(.., const TRect&)`, 56 `DrawBitmap(const TRect&, const CFbsBitmap*)`
  (RedHood). A context created for a bitmap device (`gVtBmpGc`) stores the bitmap in word 1 of the object and draws into it.

**Usual mistakes.**
- **Wrong slot numbers.** Count from the v2 header, base class first, include every virtual (also those you do not shim).
  One missing slot shifts everything after it and the symptom is a "random" function being called.
- `iVptr` not the first word, or the facade smaller than the fields old inline code reads (allocate zeroed and generous:
  `schedshim.cpp:KFakeIdleSize`).
- **The game deletes the object inside one of its own callbacks.** Then your callback code returns into freed memory. Keep
  "in callback" flags and free later: `audioshim.cpp:TFakeStream` (`iInRunL`, `iInCb`, `iZombie`),
  `audioshim.cpp:DeferDestroy`.
- Using the object from another thread. The real v3 object belongs to the scheduler of the creating thread; refuse calls from
  elsewhere (`audioshim.cpp:Mine`).
- Passing the **game's object pointer** where a subobject pointer is needed (callback interface): the vptr you read must be that
  of the interface, and the slot numbers those of the interface (`audioshim.cpp:Deliver`, slots 2-4 of the observer).
- Real v3 resource not available (no stream, no memory): do not leave the game without a way forward; degrade to a fake.
- Every v3 function that can leave on the real object is wrapped in `TRAP` (a leave is a C++ exception, see 1.4):
  `audioshim.cpp:VWriteL` (`TRAP(err, ...WriteL(...))`).

### Pattern 5 - Calling back into old code

**When to use.** The shim, a timer, an active object or a thread has to run **old code**: a game callback, a virtual function of
a game object, a thread function. Old code has no unwind info and may *leave* (old style), so it must be entered through
`TOldRt`, which sets up an old-style trap and returns the leave reason to you.

**Skeleton.**
```cpp
TInt result = 0;
const TInt reason = TOldRt::Call1((TAny*)fn, (TInt)obj, result);                  // fn(obj)           ; result = r0
// up to four words (this + three arguments):
const TInt reason2 = TOldRt::CallArgs((TAny*)fn, (TInt)obj, a1, a2, a3, result);  // fn(obj,a1,a2,a3)
if (reason != KErrNone)
    {
    // the old code LEFT with 'reason' (a normal Symbian error code): log it, stop, or turn it into a v3 leave
    // here, because these are plain v3 frames again
    }
```
Where `fn` comes from:
- a **virtual function of a game object**: read the vptr and the slot, after checking both
  (`activeshim.cpp:OldSlot` returns 0 for a zero or misaligned vptr; `audioshim.cpp:Deliver` checks `!obs || !obs[0]`);
- a **stored callback** (`TCallBack` of `CIdle::Start`): keep `{fn, ptr}` in a context and call it from your thunk
  (`schedshim.cpp:TIdleCtx`, `schedshim.cpp:IdleThunk`).

**Real examples.**
- Virtual function of a game object: `activeshim.cpp:CGameActive::RunL`, `activeshim.cpp:CGameActive::DoCancel`; with arguments
  `activeshim.cpp:CGameActive::RunError`, `audioshim.cpp:Deliver`, `ctlreg.cpp:CtlReg_CallForeground`.
- A callback with a result that decides whether to run again: `schedshim.cpp:IdleThunk`.
- The **thread trampoline** (a new thread whose body is old code): `threadshim.cpp:ThreadEntry`. The order matters:
  `CTrapCleanup::New` -> `TOldRt::AttachThread` -> `TRAP(Call1(...))` -> `TOldRt::DetachThread` -> `delete cleanup`.
- The host's own calls of game entry points are wrapped in `gfxfake.h:TGameCallScope` (`lhgui.cpp:CLhContainer`, around the key events it offers to the game).

**What happens after the call.** The old frames are gone, so you may **now** leave the v3 way: `activeshim.cpp:CGameActive::RunL`
turns an old leave into `User::Leave(reason)` and the real scheduler calls `RunError`. The host's per-frame hook
(`schedshim.h:SchedShim_SetAfterCallHook`) runs here for the same reason: a pending `CEikAppUi::Exit()` can finally leave.

**Usual mistakes.**
- **Calling the pointer directly** (`((void(*)(TAny*))fn)(obj)`). It works until the game leaves or hits an unresolved import,
  then the leave has no old trap to return to and the host dies. Always `TOldRt::Call*`.
- Calling from a thread that is not attached: `OLDRT 2` panic. Callbacks of v3 objects arrive in the thread that created them;
  that thread is attached if it is the UI thread or a game thread created by `RThread::Create`.
- More than four words of arguments: not supported by `CallArgs`. Extend `oldrt.cpp:KCallACode` or pass a pointer to a block.
- Ignoring `reason`. At least log it (a few times), otherwise a game stuck in a loop of failures looks like silence.
- Re-entrancy: the old code deleted your object while it was running (pattern 4, zombie flag), or called back into the shim that
  called it. Mark "in call" before and clear after.
- Measuring a long game frame on the UI thread without `TGameCallScope` (`gfxfake.h:TGameCallScope`), so the watchdog cannot see it.
- Entering old code while holding a lock the old code's shims also take: deadlock. Release your lock before `TOldRt::Call*`
  (`schedshim.cpp:SchedFakeDtor` takes the lock only around the table).

### Pattern 6 - Doubles and 64-bit integers

**When to use.** A `double`, a `float`, or a 64-bit integer is an argument or a result, or old code stores a 64-bit value in an
object. Never bind these directly (rule R1 in `directbind.cpp`).

**The old layout.** A `double` is two words: **r0 = high word, r1 = low word** (`rtshim.h`: "verified with the literal 131070.0").
EABI wants low first, and EABI aligns 64-bit parameters to an even register. So **never declare a `TReal64` parameter or result**;
take the words as `TUint32` and convert in both directions. A result goes back as a `TUint64` whose halves are swapped, so that
EABI puts the *high* word in r0.

**Skeleton.**
```cpp
static TUint64 ShimCos(TUint32 aHi, TUint32 aLo)            // old: double cos(double)
    {
    TReal64 r = 0.0;                                         // Math::Cos leaves r untouched on an invalid argument
    Math::Cos(r, OldToDouble(aHi, aLo));
    return DoubleToOld(r);
    }
// two doubles in:  (TUint32 aAh, TUint32 aAl, TUint32 aBh, TUint32 aBl)   -> ShimMuldf3
// int in, double out: ShimFloatsidf;  double in, int out: ShimFixdfsi;  64-bit integer in memory: Ld64 / St64
```
with the two helpers of `rtshim.cpp:OldToDouble` and `rtshim.cpp:DoubleToOld` (use them as they are; if a second file needs them,
move them into a header, do not copy them).

**Real examples.** Arithmetic helpers `rtshim.cpp:ShimMuldf3`, `rtshim.cpp:ShimDivdf3`, `rtshim.cpp:ShimAdddf3`,
`rtshim.cpp:ShimNegdf2`, `rtshim.cpp:ShimFloatsidf`, `rtshim.cpp:ShimFixdfsi` (clamps, NaN gives 0); library `rtshim.cpp:ShimCos`,
`rtshim.cpp:ShimSin`; a seed stored as a `TInt64` in an old object `rtshim.cpp:ShimFRand` (work on a local copy); 64-bit
division `rtshim.cpp:ShimDivdi3`. `TInt64` objects: `compatshim.cpp:ShimTInt64Ctor`, `compatshim.cpp:ShimTInt64Sub`
(hidden result pointer, this, rhs), `compatshim.cpp:ShimTInt64Less`, `compatshim.cpp:ShimHomeTime` (two 32-bit stores into a
possibly 4-byte-aligned destination), `compatshim.cpp:ShimMicroSecondsFrom` (64-bit by value after `this`, as two `TUint32`).

**Usual mistakes.**
- A `TReal64` parameter or result: the words arrive swapped and the answer is plausible-looking garbage.
- Returning `DoubleToOld(x)` as a `TReal64`: the compiler moves it into the wrong registers.
- Dereferencing a `TInt64*` or `TReal64*` that points into an old object: the comments of `compatshim.cpp` and `rtshim.cpp` say such
  objects may be only 4-byte aligned, so a 64-bit access is not safe. Copy two 32-bit words (`compatshim.cpp:Ld64`,
  `compatshim.cpp:St64`).
- Assuming the old and new rules for a 64-bit value that is **not first**: old packs it right after the previous argument.
- Division by zero and NaN: decide, and write the decision in the comment (`rtshim.cpp:ShimDivdf3` returns 0.0 where the old
  soft-float would trap, so the game keeps running).
- Checking only one value. A value whose low word is 0 (131070.0 is `40ffffe0 00000000`) cannot show a word swap in the low half:
  also try one with a non-zero low word, a negative one, and zero.

### Pattern 7 - Destructors and base-class shims

**When to use.** The game's own destructor calls a **base class destructor** that is an import (`CBase::~CBase`,
`CActive::~CActive`, `CApplication::~...`), or the game deletes an object **through word 2 of a vtable** you gave it.

**The old convention.** The destructor is called as `dtor(this, flags)`. `flags & 1` means "also free the memory"; the derived
destructor passes its flags on (3 in the logs). Whoever is the **last in the chain** (the `CBase` root) frees the object; the
others must not.

**Skeleton.**
```cpp
static void ShimFooDtor(TAny* aThis, TInt aFlags)           // base class destructor, or vtable word 2
    {
    // 1. release what the shim owns for aThis: side-table entry first (under the lock), then the real object
    // 2. only the root of the chain frees the memory:
    if ((aFlags & 1) && aThis)
        {
        User::Free(aThis);
        }
    }
```
**Real examples.** The root: `rtshim.cpp:ShimCBaseDtor`. Do nothing, the game frees itself: `eikshim.cpp:ShimBaseDtorNoop`,
`lhgui.cpp:ShimBaseDtorNoop`. Release a paired real object: `activeshim.cpp:ShimActiveDtor`, `schedshim.cpp:IdleFakeDtor`,
`schedshim.cpp:SchedFakeDtor`. Deleting a v3 object the old code knows only through a facade: `fsshim.cpp:DirFakeDtor`.
How the host destroys an **old** object (it calls the old destructor, never the v3 one): `memshim.cpp:OldDeleteCBase`.

**Usual mistakes.**
- Freeing twice (two base destructors both honouring bit 0) or never (nobody honours it). Find which import is the root in the
  disassembly of the game's destructor.
- Calling a v3 destructor, or `delete`, on an old object: the old vptr is not a v3 vptr.
- Deleting the real object **from another thread** (the real active object or idle belongs to the scheduler of its thread):
  check the owner thread and leak rather than crash, with a log line (`activeshim.cpp:ShimActiveDtor`).
- Deleting inside a callback that you are still running (pattern 4, zombie flag).
- Forgetting the side-table entry: a later object **at the same address** finds the stale entry (`schedshim.cpp:ShimIdleCtor`
  reuses a slot with the same `iObj`, and every destructor clears its entry).

### Pattern 8 - Leaving

**When to use.** The old function leaves on failure (`...L` functions, `User::Leave`, `LeaveIfError`, `new (ELeave)`), or you call a
v3 function that can leave.

**Skeleton.**
```cpp
// old: static CFoo* CFoo::NewL(TInt aSize)  -- leaves on failure
static TAny* ShimFooNewL(TInt aSize)
    {
    CFoo* p = NULL;
    TInt err = KErrNone;
    TRAP(err, p = CFoo::NewL(aSize));      // the v3 leave (a C++ exception) is caught HERE, in v3 frames
    if (err != KErrNone)
        {
        TOldRt::Leave(err);                // old-style leave to the innermost old trap; never returns
        }
    return p;
    }
```
A shim that detects the failure itself skips the `TRAP` (`memshim.cpp:ShimAllocL`: `if (!p) TOldRt::Leave(KErrNoMemory);`).

**Real examples.** `memshim.cpp:ShimHBufC8NewL` (the skeleton, nearly verbatim), `memshim.cpp:ShimAllocL`,
`memshim.cpp:ShimCBaseNewL`, `memshim.cpp:ShimLeaveIfNull`, `memshim.cpp:ShimLeaveIfError`, `memshim.cpp:ShimLeave` (old
`User::Leave` itself), `audioshim.cpp:ShimAudioNewL`, `audioshim.cpp:VWriteL` (queue full leaves `KErrOverflow`).

**What `TOldRt::Leave` does** (`oldrt.cpp:DoLeave`): pops the trap frame, **destroys the cleanup items the old code pushed since the
trap** (it counts them: `TOldRt::NotePush` / `NotePop`, see `memshim.cpp:ShimPushL`), stores the reason where the old `TRAP` wants it
and longjmps back. `TOldRt::Abandon` is the same but only *discards* the items: for deliberate stops by the host
(`iatfill.cpp:TrapHandler` uses it when an unresolved import is hit and tracing is off).

**Exit by leaving.** `CEikAppUi::Exit()` ends an app by leaving, which must not travel through old frames. The shim records the
request, unwinds the old frames with an old leave, and the host performs the real `Exit()` afterwards in v3 frames:
`lhgui.cpp:ShimAppUiExit`, `lhgui.cpp:HostDoPendingExitL`. Note the guard `if (TOldRt::State().iTrapTop)`.

**Usual mistakes.**
- `User::Leave()`, `User::LeaveIfError()`, `new (ELeave)` or any leaving v3 function **inside a shim without `TRAP`**: the
  exception cannot pass the old frames (they have no unwind info) and the host dies.
- `TOldRt::Leave` with **no old frame** to return to: panic `OLDLEAVE 1`. A shim that can be reached from plain host code must check
  `TOldRt::State().iTrapTop` first, as `lhgui.cpp:ShimAppUiExit` does.
- Leaving **from inside an old destructor** that runs during a leave: the second leave would jump to the wrong frame. The state
  flag is `oldrt.h:TOldThreadState` (`iInOldDelete`); the trap stub ignores the hit then (`iatfill.cpp:TrapHandler`).
- Pushing a cleanup item **inside a TRAP**: the item is still on the stack when the TRAP level exits and the cleanup stack panics
  `E32USER-CBase 71`. That is why `memshim.cpp:ShimPushL` has no TRAP around the push.
- Using a **positive or invented reason** for a real failure. Use the Symbian error codes (`KErrNoMemory`); the host's own codes
  are `iatfill.h:KIatStubHitReason` (-9999) and `lhgui.cpp:KHostExitLeave` (-9001).
- Forgetting that `return` after `TOldRt::Leave` is dead code: the compiler needs it for the type, nothing else.

### Pattern 9 - Paired objects: the game's object and a real v3 object

**When to use.** The game **derives from a v3 framework class that lives in the kernel or scheduler** (`CActive`, `CIdle`,
`CActiveScheduler`, `CPeriodic`). The object belongs to the game (its memory, its own members after the base part), but the real
v3 object has to exist in the real scheduler. The shim keeps a **side table** `old object address -> real object`; the real
object calls back into the game's virtual functions through `TOldRt` (pattern 5).

**Skeleton.**
```cpp
const TInt KMaxThing = 8;                                      // fixed table: size it for the games you know, log "FAILED" on overflow
struct TThingEntry { TUint32 iOld; CRealThing* iReal; };       // iOld == 0: free slot
static TThingEntry gThing[KMaxThing];
static RFastLock gLock;  static TBool gLockReady;              // created in ThingShim_Init
static void Lock()   { if (gLockReady) { gLock.Wait(); } }
static void Unlock() { if (gLockReady) { gLock.Signal(); } }

static TAny* ShimThingCtor(TAny* aThis, TInt aPriority)        // the base constructor, called from the game's own constructor
    {
    CRealThing* real = new CRealThing(aPriority);              // belongs to THIS thread's scheduler
    TBool stored = EFalse;
    if (real)
        {
        Lock();
        for (TInt i = 0; i < KMaxThing && !stored; i++)
            {
            if (gThing[i].iOld == 0) { gThing[i].iOld = (TUint32)aThis; gThing[i].iReal = real; stored = ETrue; }
            }
        Unlock();
        if (!stored) { delete real; }
        }
    return aThis;                                              // pattern 2
    }
// every other shim: look the real object up under the lock, then work WITHOUT the lock
```
`CRealThing::RunL()` finds the game's virtual function and calls it with `TOldRt::Call1` (pattern 5).

**Real examples.** `activeshim.cpp:ShimActiveCtor`, `activeshim.cpp:gAct`, `activeshim.cpp:CGameActive`,
`activeshim.cpp:ShimActiveSetActive`, `activeshim.cpp:ActiveShim_Add`, `activeshim.cpp:ActiveShim_Cancel`; the CIdle version
`schedshim.cpp:ShimIdleCtor`, `schedshim.cpp:gDerived`, `schedshim.cpp:ShimIdleStart`, `schedshim.cpp:IdleThunk`; the scheduler
`schedshim.cpp:ShimSchedCtor`, `schedshim.cpp:gSched`. The old `iStatus` is completed through the real one: pattern 15.

**Usual mistakes.**
- Not zeroing the **old part** of the object that inline code reads (`activeshim.cpp:KOldActiveSize`: iStatus at +4, iActive at +20).
- **Threads.** The real object belongs to the scheduler of the creating thread. Remember `MyId()` and ignore (with a log line)
  `Add`, `Cancel` and delete from another thread (`activeshim.cpp:ActiveShim_Add`).
- A **lock held while calling old code or v3 code that can block**: take it only around the table.
- Table full: the object silently never runs. Log it (`FAILED, this object will not run`) and size the table for the games.
- Assuming the game's virtual slots (3 = `DoCancel`, 4 = `RunL`, 5 = `RunError`, `activeshim.cpp:KSlotRunL`): they come from the old
  class layout. A game class that overrides differently still has the same slots, but **check the vptr before reading** (`activeshim.cpp:OldSlot`).
- A request pending in a real asynchronous API (timer, socket) whose status is the **old** `iStatus`: only requests completed by
  `RThread::RequestComplete` are supported (`activeshim.h`); anything else needs a dedicated shim.

### Pattern 10 - Host-object shim

**When to use.** The answer lives in the **host**: the real AppUi, the client rectangle, the fake `CCoeEnv`, the exit request.
Such a shim cannot be pure; it belongs **next to the state it reads** (`compatshim.cpp` says: pure functions only; "a shim that needs
host state belongs next to that state").

**Skeleton.**
```cpp
static CLhAppUi* gHostAppUi = NULL;                // set by the host at start-up

static TRect ShimClientRect(TAny*)                 // old: TRect CEikAppUi::ClientRect(): hidden result pointer, this ignored
    {
    return gHostAppUi ? gHostAppUi->ProxyClientRect() : TRect();      // guard: the shim may run before/after the host
    }
```
**Real examples.** All of `lhgui.cpp:KHostShimEntries`: `lhgui.cpp:ShimClientRect`, `lhgui.cpp:ShimApplication` (returns the old application
object), `lhgui.cpp:ShimCoeEnvStatic` (returns the fake env), `lhgui.cpp:ShimKeySounds`, `lhgui.cpp:ShimNoopArgs` (the host did it
already; the game's override calls its base). `lhgui.cpp:ShimUserExit` decides by thread kind: on a game thread it ends that
thread, on the UI thread it takes the AppUi route.

**Usual mistakes.**
- No `NULL` guard on the host global (start-up and shut-down order).
- Doing the work **in the shim** that the host AppUi already did, or twice: the "base class call" shims (`ShimNoopArgs`) exist
  because the game's override calls the base and the host AppUi has already run it.
- Putting host state into `compatshim.cpp`, whose rule is that it has none.
- Touching UI objects from a **game thread**: the shim may run on either thread. Check `TOldRt::State().iIsGameThread` and hand
  the work to the UI thread when needed.

### Pattern 11 - Handle wrapper (forward through the real v3 class)

**When to use.** The old object is a **handle** (`RFs`, `RFile`, `RSemaphore`, `RMutex`: one handle word in both SDKs), and the
v3 class does the real work; only a detail changes (a path, a wait, a value that was renumbered). The shim casts `this` to the
v3 class and calls its C++ method; it does **not** use ordinals of v3.

**Skeleton.**
```cpp
static TInt ShimFileOpen(RFile* aThis, RFs* aFs, const TDesC* aName, TUint aMode)     // efsrv 121
    {
    TFileName mapped;
    const TDesC* name = aName;
    if (RemapOldPath(*aName, mapped)) { name = &mapped; }      // the one thing that differs: where the game's files are
    return aThis->Open(*aFs, *name, aMode);
    }
```
**Real examples.** `fsshim.cpp:ShimFileOpen`, `fsshim.cpp:ShimFileReplace`, `fsshim.cpp:ShimFileRead`, `fsshim.cpp:ShimFileWrite`,
`fsshim.cpp:ShimFileSize`, `fsshim.cpp:ShimFileTemp` (`TFileName` is `TBuf<256>` in both SDKs, so the buffer is used as it is),
`fsshim.cpp:ShimHandleClose` (one shim for `RFs`, `RSemaphore`, `RMutex`, `RThread` close), the path mapping `fsshim.cpp:RemapOldPath`;
a **semantic** change: `threadshim.cpp:ShimSemWait` (bounded wait on a non-game thread, so a dead game thread cannot freeze the UI);
a **renumbered enumeration**: `threadshim.cpp:ShimThreadSetPriority` (old values -30..30 and 100..500 become v3 enumerators; the
"high" ones are clamped by the profile), `compatshim.cpp:ShimHalGet`.

**Usual mistakes.**
- Binding to the **v3 ordinal** of the method instead of calling the C++ method: ordinals change between SDKs, the C++ call
  does not. That is why these are shims, not direct binds.
- A parameter that is a reference to a **bigger** v3 type (`TFileName` locals on a small stack).
- Logging every call: `fsshim.cpp:LogOp` limits each operation to a few lines.
- Assuming old absolute paths exist: `\system\apps\<game>\...` is mapped into the game's folder, other absolute paths are left alone
  (`fsshim.cpp:RemapOldPath`).
- A wait that can block the UI for ever (semaphore waited by the UI thread): bound it and stop early when no game thread is alive.

### Pattern 12 - Per-thread state, thread creation, thread-local storage

**When to use.** The shim creates a thread that will run old code, or keeps a value per thread (`Dll::SetTls`, "am I the game
thread").

**The rule.** The old runtime keeps **everything that changes per call** in `TOldThreadState` of the current thread
(`oldrt.h`): trap chain, cleanup depth, last unresolved import, "is game thread", TLS value. Do not replace it with a global:
two threads run old code at the same time.

**Skeleton (thread entry).**
```cpp
static TInt ThreadEntry(TAny* aCtx)                       // runs on the NEW thread
    {
    // take fn/param out of the context, free the context
    CTrapCleanup* cleanup = CTrapCleanup::New();           // 1. cleanup stack
    if (!cleanup) { return KErrNoMemory; }
    TInt err = TOldRt::AttachThread();                     // 2. per-thread old runtime state
    if (err == KErrNone)
        {
        TOldRt::State().iIsGameThread = ETrue;
        TInt oldResult = 0, reason = KErrNone;
        TRAP(err, reason = TOldRt::Call1((TAny*)fn, (TInt)param, oldResult));   // 3. old code
        TOldRt::DetachThread();                            // 4. state gone before the cleanup stack
        }
    delete cleanup;
    return err;
    }
```
**Real examples.** `threadshim.cpp:ThreadEntry`, `threadshim.cpp:ShimThreadCreate` (v3 thread names must be unique; the stack is never
smaller than `gMinStack`; a NULL heap so the thread shares the creator's heap; retries on `KErrTooBig`),
`threadshim.cpp:ShimDllSetTls` / `threadshim.cpp:ShimDllTls` (TLS in `TOldThreadState::iTls`), `threadshim.cpp:ShimCTrapCleanupNew` (the cleanup stack
already exists, so old code gets a fake object).

**Usual mistakes.**
- Running old code on a thread **without `AttachThread`**: `OLDRT 2`. The `TOldRt::Init()` call attaches the calling (UI) thread only.
- Detaching **after** deleting the cleanup stack, or not at all: states are a fixed table (`oldrt.cpp:KMaxOldThreads` = 8).
- A **second `CTrapCleanup`** for the thread: a thread has one cleanup stack and the trampoline already created it (that is why `ShimCTrapCleanupNew` hands old code a fake object).
- Giving the new thread its **own heap**: `TOldRt::Shutdown` frees the state of every thread from the calling thread, which is legal
  only while all threads share one heap (`threadshim.h`, `oldrt.cpp:TOldRt::Shutdown`).
- A thread stack smaller than the old frames need: see `threadshim.cpp:gMinStack` (the profile key `thread_stack_min`).
- Storing "the current game object" in a plain global that two threads can overwrite.
- Cross-thread access to a facade without an owner check (`audioshim.cpp:Mine`).

### Pattern 13 - Direct bind (no code)

**When to use.** The import is a plain function (integers and pointers), the v3 ordinal is an **exact** match in the map, and none
of the rules in the header of `directbind.cpp` is broken: no `double`/`float`/64-bit by value, no callback, no `setjmp`, no class
whose layout differs, and variadic functions only after reading the game's format strings.

**What to write.** One row, no function:
```cpp
{ "euser", 1234, 0, EShimRegDirect, "directbind", NULL },   // TFoo::Thing(int)
```
in `directbind.cpp:KDirectBindEntries`, then `python tools\check_direct.py -v`. See `docs\tools\check_direct.md`.

**Usual mistakes.**
- Listing it without an **exact EDirect** map row: the linker logs `DIRECT entry ... has no usable import map row -> trap`.
- Listing it **and** writing a shim: a second claim on the same (dll, ordinal); the linker refuses to link.
- Forgetting the **stack alignment** issue (chapter 4, mistake 9): a direct bind calls v3 code with whatever stack the old code has.
- Binding a **variadic** function without checking the format strings of every game that uses it (the VARIADIC REVIEW lines).
- Listing a function that returns a `double` while taking only integers: the table cannot see return types. Read the signature.

### Pattern 14 - Runtime-resolved entry

**When to use.** The shim's **address does not exist at build time**, for example a code stub assembled into a chunk during start-up.

**Skeleton.**
```cpp
static TUint32 ResolveThing() { return TOldRt::TrapStub(); }          // 0 until the chunk exists: the linker then uses a trap stub
{ "euser", 1148, 0, EShimRegResolvedAtRuntime, "oldrt", &ResolveThing }
```
**Real example.** `oldrt.cpp:KOldRtShimEntries` (`TTrap::Trap` and `TTrap::UnTrap`), `oldrt.cpp:ResolveTrapStub`.
**Usual mistakes.** A resolver that returns 0 for ever (logged as `has no address at link time -> trap`); resolving too early (the chunk
does not exist before `TOldRt::Init()`); forgetting that the resolver runs **at link time** on the thread that links.

### Pattern 15 - Completion across threads

**When to use.** The game completes a request status that belongs to **another thread's** active object
(`RThread::RequestComplete`).

**The mechanism.** The game's `iStatus` (offset 4 of the old object) is only a word; the real `iStatus` lives in the paired real
object (pattern 9). `RequestComplete` on the old status completes the real one, so the owning thread's real scheduler wakes up.
**Real example.** `activeshim.cpp:ShimRequestComplete`; the other side `activeshim.cpp:CGameActive`.
**Usual mistakes.** Completing a status that is **not pending** (a second completion is a stray signal: the shim checks `KRequestPending` first);
writing only the real status and not the old word the game reads (the shim writes both); a status that does not belong to a paired
object (the shim then forwards the plain `RequestComplete`, so a status word that is not an active object's still works).

## 4. The usual mistakes

Numbered so that other chapters can point at them. Each one says what you will see.

1. **`User::Leave` (or any leaving v3 call) in a shim without `TRAP`.** Old frames have no unwind information; the C++ exception
   cannot cross them. Catch it with `TRAP` and convert it: `TOldRt::Leave(err)` (pattern 8). The host dies or panics; there is no
   graceful symptom.
2. **A constructor that does not return `this`.** Old code uses the return value as the object (`obj = ctor(mem, ...)`). Return
   `aThis`, on every path, including the failure paths (pattern 2). Symptom: the game crashes later on a NULL or garbage object, far
   from the shim.
3. **A destructor that frees when it must not, or does not when it must.** Honour `flags & 1` only in the root of the chain
   (pattern 7). Symptom: a double free panic, or a slow leak.
4. **`TOldRt::Leave` with no old frame.** Panic `OLDLEAVE 1`. Check `TOldRt::State().iTrapTop` when the shim can be reached from
   host code (pattern 8).
5. **Old code on a thread that was not attached.** Panic `OLDRT 2`. Every thread that runs old code goes through
   `TOldRt::AttachThread` (pattern 12; the UI thread is attached by `TOldRt::Init`).
6. **State in a global that must be per thread.** The trap chain, "is a game thread", TLS: `TOldThreadState`. Two game threads
   overwrite a global (pattern 12). Symptom: works with one thread, random with two.
7. **A real v3 object used from another thread.** Active objects, idles, audio streams belong to their creating thread. Record the
   owner (`MyId()`) and refuse or leak with a log line (patterns 4, 9).
8. **A `double`, `float` or 64-bit value declared by its C++ type.** The old ABI passes a double as two words in the *other*
   order, and 64-bit values after other arguments are not aligned to even registers. Declare `TUint32` pairs and combine
   them yourself (pattern 6). Symptom: values that are plausible but wrong, or exactly zero.
9. **Stack alignment when old code calls v3 code directly.** EABI code may assume that `sp` is 8-byte aligned at a call; old
   (GCC 2.95) frames guarantee only 4. A shim that only moves integers does not care. Code that uses 64-bit loads/stores or
   `double`s on the stack can. The project handles it in three ways:
   - the **trap stub** realigns (`iatfill.cpp:MakeStub`: `mov r4,sp; bic sp,sp,#7`, then restores);
   - the `TOldRt` **call helpers** keep `sp` even towards the old code they call;
   - **direct binds exclude** everything with 64-bit or floating-point arguments (rule R1), and shims read 64-bit data as two
     32-bit words (`Ld64`/`St64`, pattern 6).

   There is **no generic aligning wrapper** for plain shims and direct binds, and no device failure has been traced to this. If a
   shim must call v3 code that works on 64-bit values, copy the instruction sequence of `MakeStub` rather than inventing one,
   and mention it in the row's comment.
10. **A 64-bit value read through a pointer into an old object.** The old layout may keep it 4-byte aligned; read it as two words.
11. **A vtable that is too short.** The old code calls a slot you did not fill and jumps to 0. Fill every slot you do not
    implement with the logging stub (patterns 3, 4), and count the slots from the old class, base class first.
12. **Taking `this` for the object you made.** With multiple inheritance, the pointer in `r0` can point to a base sub-object.
    Look at the offsets the old code uses in the disassembly.
13. **Deleting an object while its own callback runs.** Games delete a stream or a timer from inside `RunL` or a callback. Mark it
    as a zombie and destroy it afterwards (`audioshim.cpp:DeferDestroy`, pattern 4).
14. **Leaving or panicking while holding the lock.** Take the side-table lock around table access only; never call old code, wait,
    or leave while holding it.
15. **A side-table entry that outlives its object.** The next object at the same address finds a stale pairing. Every destructor
    clears its entry; constructors reuse an entry with the same address.
16. **A fixed table that is full.** Objects stop working silently. Log `FAILED` when you cannot store, and size tables for the
    games you have.
17. **Large locals.** Threads start with a limited stack (`gMinStack`, 0x10000 by default) and old frames sit below yours. A
    `TFileName` is 512 bytes plus; two of them in one shim is fine, a `TBuf<2048>` is not.
18. **Blocking the UI thread.** A semaphore the game never signals froze the UI once; use a bounded wait and stop early when no game
    thread is alive (`threadshim.cpp:ShimSemWait`).
19. **Log spam.** A shim called thousands of times per second must log only the first few calls (`fsshim.cpp:LogOp`, the counters
    in `activeshim.cpp`). The device log has a size limit and the baselines compare it.
20. **Registration errors.** The same (dll, ordinal) in two tables, an empty row, or a function that is also in
    `directbind.cpp`: the linker refuses to link (`KErrAlreadyExists`). A new table that is not in `KRegSteps`: nothing happens, the
    import stays a trap. A new `.cpp` that is not in the project's `.mmp` `SOURCE` list: linker error.
21. **A wrong ordinal.** The row's number is the **v2** ordinal (the one in the game's import table), not the v3 one.
22. **Copying a convention from the name.** `void Foo(TInt)` is not always what the old code does. Read the disassembly of two call
    sites before you write the signature (chapter 5, step 3).
23. **Forgetting the comment.** Every row carries the old C++ name. It is what `check_direct.py` and the next reader use.

24. **Handing out uninitialised memory.** Old code often reads memory it never wrote and got zeros from the fresh old heap
    (Qwak reads the first word of its game object, allocated with `User::AllocL`, as a key index and never sets it). Belle
    reuses freed cells, so the same game dies there with `KERN-EXEC 3` while it runs on S60 3rd. Every allocation shim for
    old code (`CBase::newL`, `CBase::operator new`, `User::AllocL`, `__builtin_new`, `__builtin_vec_new`) returns **zeroed**
    memory (`User::AllocZ`). A new allocation shim must do the same. Not covered: the new tail of a `ReAlloc`, stack
    locals, and imports bound DIRECT to a v3 allocator (`User::Alloc`, `malloc`).
    Symptom: a game that works on one phone and dies on another early, inside a function that indexes or follows a pointer
    taken from a fresh object.

25. **A guessed profile value.** `display_mode` in the profile is the **screen** mode the game expects, not a property of its
    bitmaps; `newgame.py` / `triage.py` only guess it. A game that runs (frames are counted) but shows a white or black screen:
    check `display_mode` first. RedHood needs 7; the host copies a frame to the screen only when the fake screen mode is 7 and the
    game's bitmap is mode 7 or 10 (`gfxfake.cpp:BlitToScreen`).
 
26. **Drive-less paths.** Old code often opens a name like `\system\apps\game\data.bin` or `data.bin` without a drive letter.
    `fsshim.cpp:RemapOldPath` maps the `\system\apps\<game>\...` and drive forms into the game folder, and
    `FsShim_RemapOldPath` adds `C:` to a name that has none before it does. Every shim that opens a game file (also the
    graphics loader in `gfxfake.cpp`) must go through it. A log line `fs: fopen 'c:\...' -> 'E:\symbridge\...'` shows the result.
 
27. **Writing a decoder the phone already has.** The `.mbm` loader of RedHood/Crystal-type games does not parse the file:
    `gfxfake.cpp:LoadIntoFake` calls the real v3 `CFbsBitmap::Load` and copies the rows with `GetScanLine` (4K colour) into the
    fake bitmap. It needs an `RFbsSession` in **every game thread** that loads (`EnsureFbsSession`). Prefer the system
    function wherever the old and the new data format are the same; write your own only if the system one cannot do it.

## 5. From a trap in the log to a shim

1. **Find the trap.** In the device log, the `hits #n <dll> <ord> = <count>` lines at the end list the imports the game called
   that no shim handles. With `link_report=1` in the profile, `<app>_links.txt` lists every import and what it was bound to.
   `python tools\triage.py <game> ...` prints the "Traps that matter" list, sorted by how likely they are to matter.
2. **Get the name.** `games\<id>\map.csv` (made by `triage.py` / `newgame.py`) gives the old C++ name and the proposed v3 match.
   `EDirect` rows are candidates for pattern 13; everything else is a shim.
3. **Verify the convention.** Disassemble two or three call sites in the game: how many argument words, where `this` is, is the
   result used, is it a constructor, are there doubles or 64-bit values, does it return a struct by hidden pointer. Compare with
   the table in 1.2. Offsets and sizes of old framework objects are listed, with their source, in `docs\LAYOUT_FACTS.md`.
4. **Pick the pattern** with the decision table in chapter 2. Copy the skeleton and the nearest real example.
5. **Add the row** to the XxxShim table (v2 ordinal, function, comment with the name). If it is a new table, add the `.cpp` to the
   `.mmp` and `XxxReg_Init` to `KRegSteps`.
6. **Check on the PC.** `python tools\check_all.py` (duplicates, direct rules, import map, coverage), and
   `python tools\triage.py` for the game again: the trap should have moved from "traps" to "shims".
   The sandbox has no Symbian compiler: compile in your own SDK.
7. **Run on the device.** Unresolved imports are traced while the old `ConstructL` runs (`TRACE #n dll ord args=...`, then the `hits` lines). Read the first arguments there to check your reading of the disassembly.
8. **Accept it.** `python tools\accept.py <id> <log> --update` replaces the baseline; `python tools\coverage_all.py` refreshes
   `docs\COVERAGE.md`.

## 6. Checklist (copy into the commit message)

- [ ] Signature taken from the disassembly, not from the name (this, return value, hidden pointer, doubles, 64-bit).
- [ ] Constructors return `this`; destructors honour `flags & 1` once.
- [ ] No v3 leave or panic can pass old frames: `TRAP` + `TOldRt::Leave`.
- [ ] Old code is entered only through `TOldRt::Call0/Call1/CallArgs`.
- [ ] Per-thread state is in `TOldThreadState`; real v3 objects are used on their owner thread.
- [ ] 64-bit and `double` values are read and written as two words; no `TReal64` parameters.
- [ ] Locks are held only around table access; no old call under a lock.
- [ ] Tables are bounded and log `FAILED` when full; logging is capped.
- [ ] The row has the **v2** ordinal and the old C++ name; no duplicate claim; table in `KRegSteps`; `.cpp` in the `.mmp`.
- [ ] `python tools\check_all.py` passes; `triage.py` shows the trap gone.
- [ ] A device run was accepted (`accept.py`) and `COVERAGE.md` refreshed.
