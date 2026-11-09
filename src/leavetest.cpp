#include "leavetest.h"

// ---------------------------------------------------------------------------
// Hand-assembled ARM helpers. Encodings were verified with an assembler and the
// logic was run in an ARM emulator (leave path and normal path).
// ---------------------------------------------------------------------------
// setjmp(buf):  stmia r0,{r4-r11,sp,lr} ; mov r0,#0 ; bx lr
static const TUint32 KSetJmpCode[]  = { 0xE8806FF0, 0xE3A00000, 0xE12FFF1E };
// longjmp(buf,val): cmp r1,#0 ; moveq r1,#1 ; mov r2,r1 ;
//                   ldmia r0,{r4-r11,sp,lr} ; mov r0,r2 ; bx lr
static const TUint32 KLongJmpCode[] = { 0xE3510000, 0x03A01001, 0xE1A02001,
                                        0xE8906FF0, 0xE1A00002, 0xE12FFF1E };
// call_old(fn,arg): push {r4,lr} ; mov r4,r0 ; mov r0,r1 ; blx r4 ; pop {r4,pc}
// This frame has NO unwind information, like code from an old compiler.
static const TUint32 KCallOldCode[] = { 0xE92D4010, 0xE1A04000, 0xE1A00001,
                                        0xE12FFF34, 0xE8BD8010 };

typedef TInt (*TSetJmpFn)(TInt* aBuf);
typedef void (*TLongJmpFn)(TInt* aBuf, TInt aVal);
typedef void (*TCallFn)(TAny* aFn, TInt aArg);
typedef void (*TLeafFn)(TInt aArg);

struct TTrapFrame
    {
    TInt iBuf[10];                  // r4-r11, sp, lr
    TTrapFrame* iPrev;
    volatile TInt iReason;
    TInt iDepth;                    // old-code cleanup stack depth at trap time
    };

static TSetJmpFn   gSetJmp  = NULL;
static TLongJmpFn  gLongJmp = NULL;
static TCallFn     gCallOld = NULL;
static TTrapFrame* gTrapTop = NULL;
static TInt        gCleanupDepth = 0;   // items pushed by OLD code (via shims)

// Old-style Leave: jump to the innermost old-style trap. Never returns.
static void OldLeave(TInt aReason)
    {
    TTrapFrame* f = gTrapTop;
    if (f == NULL)
        {
        User::Panic(_L("OLDLEAVE"), 1);
        }
    gTrapTop = f->iPrev;
    f->iReason = aReason;
    // Old-style Leave must destroy the cleanup items that old code pushed
    // since the trap was set (v3 does this itself only for real leaves).
    TInt n = gCleanupDepth - f->iDepth;
    gCleanupDepth = f->iDepth;
    if (n > 0)
        {
        CleanupStack::PopAndDestroy(n);
        }
    gLongJmp(f->iBuf, 1);
    }

// Runs aFn(aArg) through a frame without unwind info, under an old-style trap.
// Returns KErrNone, or the reason given to OldLeave().
static TInt RunOldTrapped(TLeafFn aFn, TInt aArg) __attribute__((noinline));
static TInt RunOldTrapped(TLeafFn aFn, TInt aArg)
    {
    TTrapFrame frame;
    frame.iPrev = gTrapTop;
    frame.iReason = 0;
    frame.iDepth = gCleanupDepth;
    gTrapTop = &frame;
    if (gSetJmp(frame.iBuf) == 0)
        {
        gCallOld((TAny*)aFn, aArg);
        gTrapTop = frame.iPrev;             // normal completion
        return KErrNone;
        }
    return frame.iReason;                   // arrived here through OldLeave()
    }

// ---- functions used as test payloads ----

static void Nothing(TInt /*aArg*/)
    {
    }

// "Old code" leaving in the old way.
static void OldStyleLeaver(TInt aReason)
    {
    OldLeave(aReason);
    }

// Shim: what the wrapper would put in place of a v3 function that can leave.
static void ShimLeaveIfError(TInt aErr)
    {
    TInt err = KErrNone;
    TRAP(err, User::LeaveIfError(aErr));
    if (err != KErrNone)
        {
        OldLeave(err);
        }
    }

// Shim around a v3 function that leaves INSIDE the v3 library (out of memory).
static void ShimAllocTooBig(TInt /*aArg*/)
    {
    TInt err = KErrNone;
    HBufC8* p = NULL;
    TRAP(err, p = HBufC8::NewL(0x4000000));     // 64 MB, more than our heap max
    delete p;
    if (err != KErrNone)
        {
        OldLeave(err);
        }
    }

// Trampoline: new-world code calling old code; converts an old-style leave
// into a real (exception based) leave.
static void CallOldFromNewL(TLeafFn aFn, TInt aArg)
    {
    TInt r = RunOldTrapped(aFn, aArg);
    if (r != KErrNone)
        {
        User::Leave(r);
        }
    }

// Used by the destructive test: a real leave thrown from inside the old frame.
static void ThrowThrough(TInt aReason)
    {
    User::Leave(aReason);
    }


// ---- cleanup stack bridge (T5) ----
static TInt gDestroyed = 0;

class CProbe : public CBase
    {
public:
    ~CProbe() { gDestroyed++; }
    };

static CProbe* gProbeA = NULL;
static CProbe* gProbeB = NULL;

// Shims for the two cleanup stack functions the game imports.
static void ShimPushL(CBase* aItem)
    {
    CleanupStack::PushL(aItem);
    gCleanupDepth++;
    }

static void ShimPop()
    {
    CleanupStack::Pop();
    gCleanupDepth--;
    }

static void OldPushThenLeave(TInt aReason)
    {
    ShimPushL(gProbeA);
    ShimPushL(gProbeB);
    OldLeave(aReason);
    }

static void OldPushPopNormal(TInt /*aArg*/)
    {
    ShimPushL(gProbeA);
    ShimPop();
    }

static void CleanupTestsL(THostLog& aLog)
    {
    TInt r = KErrNone;
    TInt e = KErrNone;
    gCleanupDepth = 0;

    // T5a: old code pushes two items, then leaves the old way
    gDestroyed = 0;
    gProbeA = new (ELeave) CProbe;
    gProbeB = new (ELeave) CProbe;
    r = RunOldTrapped(OldPushThenLeave, -4);
    aLog.Logf(_L8("T5a old push x2 + old leave -> %d (expect -4), destroyed=%d (expect 2), depth=%d (expect 0)"),
              r, gDestroyed, gCleanupDepth);

    // T5b: push and pop, no leave
    gDestroyed = 0;
    gProbeA = new (ELeave) CProbe;
    r = RunOldTrapped(OldPushPopNormal, 0);
    delete gProbeA;
    gProbeA = NULL;
    aLog.Logf(_L8("T5b push+pop, no leave -> %d (expect 0), destroyed=%d (expect 1), depth=%d (expect 0)"),
              r, gDestroyed, gCleanupDepth);

    // T5c: an item pushed by new-world code BELOW the old trap must survive
    gDestroyed = 0;
    CProbe* below = new (ELeave) CProbe;
    CleanupStack::PushL(below);
    gProbeA = new (ELeave) CProbe;
    gProbeB = new (ELeave) CProbe;
    r = RunOldTrapped(OldPushThenLeave, -4);
    aLog.Logf(_L8("T5c item below the old trap: destroyed=%d (expect 2, not 3)"), gDestroyed);
    CleanupStack::PopAndDestroy(below);
    aLog.Logf(_L8("T5c after PopAndDestroy(below): destroyed=%d (expect 3)"), gDestroyed);

    // T5d: through the trampoline into a real TRAP
    gDestroyed = 0;
    gProbeA = new (ELeave) CProbe;
    gProbeB = new (ELeave) CProbe;
    TRAP(e, CallOldFromNewL(OldPushThenLeave, -4));
    aLog.Logf(_L8("T5d trampoline caught=%d (expect -4), destroyed=%d (expect 2), depth=%d (expect 0)"),
              e, gDestroyed, gCleanupDepth);
    }

// ---------------------------------------------------------------------------

CLeaveTest* CLeaveTest::NewL(THostLog& aLog)
    {
    CLeaveTest* self = new (ELeave) CLeaveTest(aLog);
    CleanupStack::PushL(self);
    self->ConstructL();
    CleanupStack::Pop(self);
    return self;
    }

CLeaveTest::~CLeaveTest()
    {
    iChunk.Close();
    }

void CLeaveTest::ConstructL()
    {
    User::LeaveIfError(iChunk.CreateLocalCode(0x1000, 0x1000));
    TUint8* base = iChunk.Base();
    Mem::Copy(base + 0x00, KSetJmpCode,  sizeof(KSetJmpCode));
    Mem::Copy(base + 0x20, KLongJmpCode, sizeof(KLongJmpCode));
    Mem::Copy(base + 0x40, KCallOldCode, sizeof(KCallOldCode));
    User::IMB_Range(base, base + 0x1000);
    gSetJmp  = (TSetJmpFn)(base + 0x00);
    gLongJmp = (TLongJmpFn)(base + 0x20);
    gCallOld = (TCallFn)(base + 0x40);
    gTrapTop = NULL;
    iLog.Logf(_L8("leave test helpers at %08x"), (TUint32)base);
    }

void CLeaveTest::RunSafe()
    {
    iLog.Text(_L8("--- leave tests (safe) ---"));
    TInt e = KErrNone;

    // T1: baseline, v3 leave caught by a normal TRAP
    TRAP(e, User::Leave(-5));
    iLog.Logf(_L8("T1a User::Leave(-5) in TRAP -> %d (expect -5)"), e);
    TRAP(e, User::LeaveIfError(-7));
    iLog.Logf(_L8("T1b User::LeaveIfError(-7) in TRAP -> %d (expect -7)"), e);
    HBufC8* p = NULL;
    TRAP(e, p = HBufC8::NewL(0x4000000));
    delete p;
    iLog.Logf(_L8("T1c HBufC8::NewL(64MB) in TRAP -> %d (expect -4)"), e);

    // T2: old-style leave across a frame without unwind info
    TInt r = RunOldTrapped(OldStyleLeaver, -3);
    iLog.Logf(_L8("T2a old-style leave -> %d (expect -3)"), r);
    r = RunOldTrapped(Nothing, 0);
    iLog.Logf(_L8("T2b no leave -> %d (expect 0)"), r);

    // T3a: new-style leave converted to old-style by a shim
    r = RunOldTrapped(ShimLeaveIfError, -7);
    iLog.Logf(_L8("T3a shim(LeaveIfError(-7)) -> %d (expect -7)"), r);
    r = RunOldTrapped(ShimAllocTooBig, 0);
    iLog.Logf(_L8("T3b shim(NewL out of memory) -> %d (expect -4)"), r);

    // T3c: old-style leave converted to a real leave by a trampoline
    TRAP(e, CallOldFromNewL(OldStyleLeaver, -9));
    iLog.Logf(_L8("T3c trampoline, caught by TRAP -> %d (expect -9)"), e);
    TRAP(e, CallOldFromNewL(Nothing, 0));
    iLog.Logf(_L8("T3d trampoline, no leave -> %d (expect 0)"), e);

    iLog.Logf(_L8("old-style trap stack empty: %d (expect 1)"), gTrapTop == NULL);
    }

void CLeaveTest::RunUnwindCrashTest()
    {
    iLog.Text(_L8("--- T4: real leave thrown THROUGH a frame without unwind info ---"));
    iLog.Text(_L8("expected to kill the process. If the log ends here, the problem is confirmed."));
    TInt e = KErrNone;
    TRAP(e, gCallOld((TAny*)ThrowThrough, -5));
    iLog.Logf(_L8("T4 process survived, TRAP caught %d"), e);
    }

void CLeaveTest::RunCleanupTests()
    {
    iLog.Text(_L8("--- cleanup stack tests ---"));
    TInt e = KErrNone;
    TRAP(e, CleanupTestsL(iLog));
    iLog.Logf(_L8("cleanup tests finished with %d"), e);
    }
