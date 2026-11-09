#include "oldrt.h"
#include "shimreg.h"

// Hand-assembled ARM helpers (first two are the ones already verified on the phone).
// setjmp(buf):  stmia r0,{r4-r11,sp,lr} ; mov r0,#0 ; bx lr
static const TUint32 KSetJmpCode[]  = { 0xE8806FF0, 0xE3A00000, 0xE12FFF1E };
// longjmp(buf,val): cmp r1,#0 ; moveq r1,#1 ; mov r2,r1 ;
//                   ldmia r0,{r4-r11,sp,lr} ; mov r0,r2 ; bx lr
static const TUint32 KLongJmpCode[] = { 0xE3510000, 0x03A01001, 0xE1A02001,
                                        0xE8906FF0, 0xE1A00002, 0xE12FFF1E };
// call1(fn, arg, resultPtr): push {r4-r6,lr} ; mov r4,r0 ; mov r5,r2 ; mov r0,r1 ;
//                            blx r4 ; str r0,[r5] ; pop {r4-r6,pc}
// 4 registers pushed on purpose: keeps sp 8-byte aligned for the callee.
static const TUint32 KCall1Code[]   = { 0xE92D4070, 0xE1A04000, 0xE1A05002, 0xE1A00001,
                                        0xE12FFF34, 0xE5850000, 0xE8BD8070 };

// TTrap::Trap(this=r0, TInt& aResult=r1) -- runs in the OLD caller's frame:
//   push {r1,r4,r5,lr} ; mov r4,sp ; bic sp,sp,#7 ; mov r0,r1 ; ldr r5,[pc,#0x14] ; blx r5   (TrapPush(aResult))
//   mov sp,r4 ; pop {r1,r4,r5,lr}            (caller's r4,r5,lr,sp are intact again; r0 = frame*)
//   stmia r0,{r4-r11,sp,lr} ; mov r0,#0 ; bx lr
// literal (word 11) = address of TrapPush
static const TUint32 KTrapCode[] = { 0xE92D4032, 0xE1A0400D, 0xE3CDD007, 0xE1A00001,
                                     0xE59F5014, 0xE12FFF35, 0xE1A0D004, 0xE8BD4032,
                                     0xE8806FF0, 0xE3A00000, 0xE12FFF1E, 0x00000000 };

// calla(fn=r0, args=r1, resultPtr=r2): push {r4-r6,lr} ; mov r4,r0 ; mov r5,r2 ; mov r6,r1 ;
//                                      ldmia r6,{r0-r3} ; blx r4 ; str r0,[r5] ; pop {r4-r6,pc}
static const TUint32 KCallACode[]   = { 0xE92D4070, 0xE1A04000, 0xE1A05002, 0xE1A06001,
                                        0xE896000F, 0xE12FFF34, 0xE5850000, 0xE8BD8070 };
typedef void (*TCallAFn)(TAny* aFn, const TInt* aArgs, TInt* aResult);
static TCallAFn gCallA = NULL;

typedef TInt (*TSetJmpFn)(TInt* aBuf);
typedef void (*TLongJmpFn)(TInt* aBuf, TInt aVal);
typedef void (*TCall1Fn)(TAny* aFn, TInt aArg, TInt* aResult);

const TInt KMaxOldThreads = 8;

static RChunk          gChunk;
static TSetJmpFn       gSetJmp   = NULL;
static TLongJmpFn      gLongJmp  = NULL;
static TCall1Fn        gCall1    = NULL;
static RFastLock       gLock;
static TBool           gLockReady = EFalse;
static TOldThreadState* gStates[KMaxOldThreads];

static TOldTrapFrame* TrapPush(TInt* aResultPtr)
    {
    TOldThreadState& st = TOldRt::State();
    TOldTrapFrame* f = (TOldTrapFrame*)User::AllocZ(sizeof(TOldTrapFrame));
    if (!f)
        {
        User::Panic(_L("OLDTRAP"), 1);
        }
    f->iPrev = st.iTrapTop;
    f->iDepth = st.iCleanupDepth;
    f->iResultPtr = aResultPtr;
    if (aResultPtr)
        {
        *aResultPtr = 0;            // old TTrap::Trap reports KErrNone on the normal (non-leaving) return
        }
    f->iHeap = ETrue;
    st.iTrapTop = f;
    return f;
    }

static void ShimUnTrap()                                // euser 1164: static void TTrap::UnTrap()
    {
    TOldThreadState& st = TOldRt::State();
    TOldTrapFrame* f = st.iTrapTop;
    if (f && f->iHeap)
        {
        st.iTrapTop = f->iPrev;
        User::Free(f);
        }
    }

TUint32 TOldRt::TrapStub()
    {
    return gChunk.Base() ? (TUint32)(gChunk.Base() + 0x80) : 0;
    }

TUint32 TOldRt::UnTrapFn()
    {
    return (TUint32)&ShimUnTrap;
    }

TInt TOldRt::Init()
    {
    if (gSetJmp)
        {
        return KErrNone;
        }
    if (!gLockReady)
        {
        TInt lerr = gLock.CreateLocal();
        if (lerr != KErrNone)
            {
            return lerr;
            }
        gLockReady = ETrue;
        }
    TInt err = gChunk.CreateLocalCode(0x1000, 0x1000);
    if (err != KErrNone)
        {
        return err;
        }
    TUint8* base = gChunk.Base();
    Mem::Copy(base + 0x00, KSetJmpCode,  sizeof(KSetJmpCode));
    Mem::Copy(base + 0x20, KLongJmpCode, sizeof(KLongJmpCode));
    Mem::Copy(base + 0x40, KCall1Code,   sizeof(KCall1Code));
    Mem::Copy(base + 0x80, KTrapCode,    sizeof(KTrapCode));
    *(TUint32*)(base + 0x80 + 0x2C) = (TUint32)&TrapPush;
    Mem::Copy(base + 0xC0, KCallACode,   sizeof(KCallACode));
    gCallA = (TCallAFn)(base + 0xC0);
    User::IMB_Range(base, base + 0x1000);
    gSetJmp  = (TSetJmpFn)(base + 0x00);
    gLongJmp = (TLongJmpFn)(base + 0x20);
    gCall1   = (TCall1Fn)(base + 0x40);
    return AttachThread();
    }

// Shutdown frees the state of EVERY thread from the calling thread. That is only legal while all
// threads share one heap: the game thread is created with a NULL heap (it uses the creator's heap),
// and every thread normally detaches itself. ThreadShim_Shutdown() must have stopped the game
// threads before this runs; whatever is left is freed here.
void TOldRt::Shutdown()
    {
    gSetJmp = NULL;
    gLongJmp = NULL;
    gCall1 = NULL;
    gCallA = NULL;
    if (gLockReady)
        {
        gLock.Wait();
        for (TInt i = 0; i < KMaxOldThreads; i++)
            {
            User::Free(gStates[i]);
            gStates[i] = NULL;
            }
        gLock.Signal();
        gLock.Close();
        gLockReady = EFalse;
        }
    gChunk.Close();
    }

TInt TOldRt::AttachThread()
    {
    RThread me;
    const TUint64 id = me.Id().Id();
    TInt result = KErrNone;
    gLock.Wait();
    TInt freeSlot = -1;
    TBool found = EFalse;
    for (TInt i = 0; i < KMaxOldThreads && !found; i++)
        {
        if (gStates[i])
            {
            found = (gStates[i]->iThreadId == id);
            }
        else if (freeSlot < 0)
            {
            freeSlot = i;
            }
        }
    if (!found)
        {
        if (freeSlot < 0)
            {
            result = KErrOverflow;
            }
        else
            {
            TOldThreadState* st = (TOldThreadState*)User::AllocZ(sizeof(TOldThreadState));
            if (!st)
                {
                result = KErrNoMemory;
                }
            else
                {
                st->iThreadId = id;
                st->iHit = -1;
                gStates[freeSlot] = st;
                }
            }
        }
    gLock.Signal();
    return result;
    }

void TOldRt::DetachThread()
    {
    RThread me;
    const TUint64 id = me.Id().Id();
    gLock.Wait();
    for (TInt i = 0; i < KMaxOldThreads; i++)
        {
        if (gStates[i] && gStates[i]->iThreadId == id)
            {
            User::Free(gStates[i]);
            gStates[i] = NULL;
            }
        }
    gLock.Signal();
    }

TInt TOldRt::ThreadInfo(TUint64 aThreadId, TInt& aHit, TInt& aCleanupDepth)
    {
    TInt r = KErrNotFound;
    gLock.Wait();
    for (TInt i = 0; i < KMaxOldThreads; i++)
        {
        if (gStates[i] && gStates[i]->iThreadId == aThreadId)
            {
            aHit = gStates[i]->iHit;
            aCleanupDepth = gStates[i]->iCleanupDepth;
            r = KErrNone;
            }
        }
    gLock.Signal();
    return r;
    }

TOldThreadState& TOldRt::State()
    {
    RThread me;
    const TUint64 id = me.Id().Id();
    TOldThreadState* found = NULL;
    gLock.Wait();
    for (TInt i = 0; i < KMaxOldThreads && !found; i++)
        {
        if (gStates[i] && gStates[i]->iThreadId == id)
            {
            found = gStates[i];
            }
        }
    gLock.Signal();
    if (!found)
        {
        User::Panic(_L("OLDRT"), 2);
        }
    return *found;
    }

void TOldRt::NotePush()
    {
    State().iCleanupDepth++;
    }

void TOldRt::NotePop()
    {
    State().iCleanupDepth--;
    }

static void DoLeave(TInt aReason, TBool aDestroy)
    {
    TOldThreadState& st = TOldRt::State();
    TOldTrapFrame* f = st.iTrapTop;
    if (f == NULL)
        {
        User::Panic(_L("OLDLEAVE"), 1);
        }
    st.iTrapTop = f->iPrev;
    f->iReason = aReason;
    TInt n = st.iCleanupDepth - f->iDepth;
    st.iCleanupDepth = f->iDepth;
    if (n > 0)
        {
        if (aDestroy)
            {
            CleanupStack::PopAndDestroy(n);
            }
        else
            {
            CleanupStack::Pop(n);
            }
        }
    if (f->iHeap)
        {
        TInt buf[10];                       // the frame is freed before the jump
        Mem::Copy(buf, f->iBuf, sizeof(buf));
        if (f->iResultPtr)
            {
            *f->iResultPtr = aReason;
            }
        User::Free(f);
        gLongJmp(buf, 1);
        }
    gLongJmp(f->iBuf, 1);
    }

void TOldRt::Leave(TInt aReason)   { DoLeave(aReason, ETrue); }
void TOldRt::Abandon(TInt aReason) { DoLeave(aReason, EFalse); }

// Not inlined, so that the frame holding the jump buffer is a real frame.
static TInt CallOldTrapped(TAny* aFn, TInt aArg, TInt& aResult, const TInt* aArgs) __attribute__((noinline));
static TInt CallOldTrapped(TAny* aFn, TInt aArg, TInt& aResult, const TInt* aArgs)
    {
    TInt args[4];
    if (aArgs)
        {
        Mem::Copy(args, aArgs, sizeof(args));
        }
    TOldThreadState* st = &TOldRt::State();
    TOldTrapFrame frame;
    frame.iPrev = st->iTrapTop;
    frame.iReason = 0;
    frame.iDepth = st->iCleanupDepth;
    frame.iResultPtr = NULL;
    frame.iHeap = EFalse;
    st->iTrapTop = &frame;
    if (gSetJmp(frame.iBuf) == 0)
        {
        if (aArgs)
            {
            gCallA(aFn, args, &st->iCallResult);
            }
        else
            {
            gCall1(aFn, aArg, &st->iCallResult);
            }
        st->iTrapTop = frame.iPrev;         // normal completion
        aResult = st->iCallResult;
        return KErrNone;
        }
    return frame.iReason;                   // arrived here through Leave()
    }

TInt TOldRt::Call0(TAny* aFn, TInt& aResult)
    {
    return CallOldTrapped(aFn, 0, aResult, NULL);
    }

TInt TOldRt::Call1(TAny* aFn, TInt aArg, TInt& aResult)
    {
    return CallOldTrapped(aFn, aArg, aResult, NULL);
    }

TInt TOldRt::CallArgs(TAny* aFn, TInt a0, TInt a1, TInt a2, TInt a3, TInt& aResult)
    {
    const TInt args[4] = { a0, a1, a2, a3 };
    return CallOldTrapped(aFn, 0, aResult, args);
    }

// euser 1148 TTrap::Trap: the stub lives in the helper code chunk, which only exists after TOldRt::Init(),
// so the address is resolved when the game is linked. 0 until then (the linker then uses a trap stub).
// euser 1164 TTrap::UnTrap: a fixed function in this file; it goes through a resolver too so both rows read alike.
static TUint32 ResolveUnTrap()
    {
    return TOldRt::UnTrapFn();
    }

static TUint32 ResolveTrapStub()
    {
    return TOldRt::TrapStub();
    }

static const TShimRegEntry KOldRtShimEntries[] =
    {
    { "euser", 1148, 0, EShimRegResolvedAtRuntime, "oldrt", &ResolveTrapStub },   // TTrap::Trap(TInt&)
    { "euser", 1164, 0, EShimRegResolvedAtRuntime, "oldrt", &ResolveUnTrap }      // TTrap::UnTrap()
    };

TInt OldRtShim_Init()
    {
    return ShimReg_Add(KOldRtShimEntries, sizeof(KOldRtShimEntries) / sizeof(KOldRtShimEntries[0]));
    }
