// The active scheduler family of the old game: CActiveScheduler, CIdle (also as a base class of game classes),
// CPeriodic, CActive::Cancel and User::After. The old objects stay the game's facades; the real v3 objects
// live beside them (side tables / hidden pointers), and every callback into old code goes through TOldRt.
#include <e32base.h>
#include "schedshim.h"
#include "oldrt.h"
#include "shimreg.h"
#include "activeshim.h"
#include "gfxfake.h"
#include "crashwatch.h"

static THostLog* gLog = NULL;
static RFastLock gLock;
static TBool gLockReady = EFalse;

static void Lock()   { if (gLockReady) { gLock.Wait(); } }
static void Unlock() { if (gLockReady) { gLock.Signal(); } }

// ---------------------------------------------------------------------------
// old CActiveScheduler objects
// ---------------------------------------------------------------------------
const TInt KMaxSched = 8;

struct TSchedEntry
    {
    TUint32 iOld;                   // address of the old object (0 = free slot)
    CActiveScheduler* iReal;
    TUint64 iThread;                // thread that created it (and that Installs and runs it)
    };

static TSchedEntry gSched[KMaxSched];

static TUint64 MyId()
    {
    RThread me;
    return me.Id().Id();
    }

static TSchedEntry* FindSched(TUint32 aOld)         // call with the lock held
    {
    for (TInt i = 0; i < KMaxSched; i++)
        {
        if (gSched[i].iOld == aOld)
            {
            return &gSched[i];
            }
        }
    return NULL;
    }

// Old-style (GCC 2.9) vtables of the fake objects: two header words, then the slots; the
// destructor is word 2 and is called as dtor(this, flags), flags & 1 = also free the memory.
static TUint32 gSchedVt[8];
static TUint32 gIdleVt[8];
static TInt gSlotLogs = 0;

static TInt FakeSlot()
    {
    if (gLog && gSlotLogs < 6)
        {
        gSlotLogs++;
        gLog->Text(_L8("sched: unexpected virtual call on a fake scheduler/idle object (returns 0)"));
        }
    return 0;
    }

static void SchedFakeDtor(TAny* aThis, TInt aFlags)
    {
    CActiveScheduler* real = NULL;
    Lock();
    TSchedEntry* e = FindSched((TUint32)aThis);
    if (e)
        {
        if (e->iThread == MyId())
            {
            real = e->iReal;
            }
        else if (gLog)
            {
            gLog->Text(_L8("sched: scheduler deleted from another thread, real one leaked"));
            }
        e->iOld = 0;
        e->iReal = NULL;
        }
    Unlock();
    if (real)
        {
        if (CActiveScheduler::Current() == real)
            {
            CActiveScheduler::Install(NULL);
            }
        delete real;
        }
    if (gLog)
        {
        gLog->Logf(_L8("sched: old scheduler %08x destroyed (flags %d)"), (TUint32)aThis, aFlags);
        }
    if (aFlags & 1)
        {
        User::Free(aThis);
        }
    }

// euser 1343: CActiveScheduler::CActiveScheduler()
static TAny* ShimSchedCtor(TUint32* aThis)
    {
    CActiveScheduler* real = new CActiveScheduler;
    aThis[0] = (TUint32)gSchedVt;       // a derived old class overwrites this with its own vptr
    TBool stored = EFalse;
    if (real)
        {
        Lock();
        for (TInt i = 0; i < KMaxSched && !stored; i++)
            {
            if (gSched[i].iOld == 0)
                {
                gSched[i].iOld = (TUint32)aThis;
                gSched[i].iReal = real;
                gSched[i].iThread = MyId();
                stored = ETrue;
                }
            }
        Unlock();
        if (!stored)
            {
            delete real;
            real = NULL;
            }
        }
    if (gLog)
        {
        gLog->Logf(_L8("sched: CActiveScheduler ctor old=%08x real=%08x"), (TUint32)aThis, (TUint32)real);
        }
    return aThis;
    }

// euser 569: static void CActiveScheduler::Install(CActiveScheduler*)
static void ShimSchedInstall(TUint32 aOld)
    {
    if (aOld == 0)
        {
        CActiveScheduler::Install(NULL);
        if (gLog)
            {
            gLog->Text(_L8("sched: Install(NULL)"));
            }
        return;
        }
    CActiveScheduler* real = NULL;
    Lock();
    TSchedEntry* e = FindSched(aOld);
    if (e)
        {
        real = e->iReal;
        }
    Unlock();
    if (real)
        {
        CActiveScheduler::Install(real);
        }
    if (gLog)
        {
        gLog->Logf(_L8("sched: Install(old=%08x) real=%08x"), aOld, (TUint32)real);
        }
    }

// euser 1107: static void CActiveScheduler::Start()
static void ShimSchedStart()
    {
    if (CActiveScheduler::Current() == NULL)
        {
        if (gLog)
            {
            gLog->Text(_L8("sched: Start without an installed scheduler, ignored"));
            }
        return;
        }
    if (gLog)
        {
        gLog->Text(_L8("sched: Start (entering the real loop)"));
        }
    CActiveScheduler::Start();
    if (gLog)
        {
        gLog->Text(_L8("sched: Start returned (Stop was called)"));
        }
    }

// euser 1113: static void CActiveScheduler::Stop()
static void ShimSchedStop()
    {
    if (gLog)
        {
        gLog->Text(_L8("sched: Stop"));
        }
    if (CActiveScheduler::Current())
        {
        CActiveScheduler::Stop();
        }
    }

// ---------------------------------------------------------------------------
// old CIdle objects: fake object { vptr, real CIdle*, callback context*, spare ... }
// ---------------------------------------------------------------------------
const TInt KFakeIdleSize = 0x40;            // zeroed, so inline reads of CActive fields see "idle"

struct TIdleCtx
    {
    TInt (*iFn)(TAny*);                     // old code
    TAny* iPtr;
    };

static TInt gIdleLogs = 0;

static void (*gAfterCallHook)() = NULL;      // host callback after every run of a game callback (see schedshim.h)

// ---------------------------------------------------------------------------
// old CPeriodic objects: fake object { vptr, real CPeriodic*, callback context*, ... }
// ---------------------------------------------------------------------------
static TUint32 gPeriodicVt[8];
static TInt gPeriodicLogs = 0;

static TInt gMinIntervalUs = 0;          // max_fps from configs.cfg: shortest time between two runs, 0 = no limit
static TInt64 gLastRunUs = 0;

// max_fps: a game whose frame loop has no timing of its own is slowed down here, before each run of its
// CIdle / CPeriodic callback. The wait uses the 1 ms timer, not the 15.6 ms tick.
static void LimitFrameRate()
    {
    if (gMinIntervalUs <= 0)
        {
        return;
        }
    TTime now;
    now.UniversalTime();
    const TInt64 elapsed = now.Int64() - gLastRunUs;
    if (gLastRunUs != 0 && elapsed >= 0 && elapsed < gMinIntervalUs)
        {
        User::AfterHighRes(TTimeIntervalMicroSeconds32((TInt)(gMinIntervalUs - elapsed)));
        now.UniversalTime();
        }
    gLastRunUs = now.Int64();
    }

static TInt PeriodicThunk(TAny* aCtx)
    {
    TIdleCtx* c = (TIdleCtx*)aCtx;          // same context as the CIdle one
    TInt result = 0;
    LimitFrameRate();
    TInt reason;
        {
        TGameCallScope scope;
        reason = TOldRt::Call1((TAny*)c->iFn, (TInt)c->iPtr, result);
        }
    if (gAfterCallHook)
        {
        gAfterCallHook();                   // may leave (pending host exit); runs in plain v3 frames
        }
    if (reason != KErrNone && gLog && gPeriodicLogs < 4)
        {
        gPeriodicLogs++;
        gLog->Logf(_L8("periodic: callback left with %d"), reason);
        }
    return 0;                               // CPeriodic ignores the result
    }

static void PeriodicFakeDtor(TAny* aThis, TInt aFlags)
    {
    TUint32* o = (TUint32*)aThis;
    CPeriodic* real = (CPeriodic*)o[1];
    if (real)
        {
        real->Cancel();
        delete real;
        o[1] = 0;
        }
    User::Free((TAny*)o[2]);                // callback context
    o[2] = 0;
    if (aFlags & 1)
        {
        User::Free(aThis);
        }
    }

// euser 752: static CPeriodic* CPeriodic::New(TInt aPriority)
static TAny* ShimPeriodicNew(TInt aPriority)
    {
    CPeriodic* real = CPeriodic::New(aPriority);
    if (real && !real->IsAdded())           // defensive, same rule as for CIdle / CTimer
        {
        CActiveScheduler::Add(real);
        }
    TUint32* o = NULL;
    if (real)
        {
        o = (TUint32*)User::AllocZ(KFakeIdleSize);
        if (o)
            {
            o[0] = (TUint32)gPeriodicVt;
            o[1] = (TUint32)real;
            }
        else
            {
            delete real;
            }
        }
    if (gLog)
        {
        gLog->Logf(_L8("periodic: CPeriodic::New(priority %d) -> fake %08x"), aPriority, (TUint32)o);
        }
    return o;
    }

// euser 738: static CPeriodic* CPeriodic::NewL(TInt aPriority) -- like New, but leaves when out of memory.
static TAny* ShimPeriodicNewL(TInt aPriority)
    {
    TAny* o = ShimPeriodicNew(aPriority);
    if (!o)
        {
        TOldRt::Leave(KErrNoMemory);
        }
    return o;
    }

// euser 1111: void CPeriodic::Start(TTimeIntervalMicroSeconds32 aDelay, TTimeIntervalMicroSeconds32 aInterval, TCallBack)
static void ShimPeriodicStart(TAny* aThis, TInt aDelay, TInt aInterval, TInt (*aFn)(TAny*), TAny* aPtr)
    {
    TUint32* o = (TUint32*)aThis;
    if (!o || o[0] != (TUint32)gPeriodicVt || !o[1])
        {
        if (gLog)
            {
            gLog->Logf(_L8("periodic: Start on %08x which is not a fake CPeriodic, ignored"), (TUint32)aThis);
            }
        return;
        }
    CPeriodic* real = (CPeriodic*)o[1];
    TInt delay = aDelay < 0 ? 0 : aDelay;
    TInt interval = aInterval;
	if (interval <= 0)
	{
	interval = 15625;                   // asked 0 = as fast as possible: on the old kernel one system tick (1/64 s)
	}
	else if (interval < 1000)
	{
	interval = 1000;                    // 1 ms floor: an odd interval would starve the host's own timers
	}
    TIdleCtx* ctx = (TIdleCtx*)User::AllocZ(sizeof(TIdleCtx));
    if (!ctx)
        {
        return;
        }
    ctx->iFn = aFn;
    ctx->iPtr = aPtr;
    real->Cancel();
    User::Free((TAny*)o[2]);
    o[2] = (TUint32)ctx;
    real->Start(TTimeIntervalMicroSeconds32(delay), TTimeIntervalMicroSeconds32(interval), TCallBack(PeriodicThunk, ctx));
    if (gLog)
        {
        gLog->Logf(_L8("periodic: Start(delay %d us, interval %d us (asked %d), fn=%08x, ptr=%08x)"),
                   delay, interval, aInterval, (TUint32)aFn, (TUint32)aPtr);
        }
    }

static TInt gIdleSeq = 0;
static TInt IdleThunk(TAny* aCtx)
    {
    TIdleCtx* c = (TIdleCtx*)aCtx;
    TInt result = 0;
    LimitFrameRate();
    TInt reason;
	gIdleSeq++;
        {
        TGameCallScope scope;
        CRUMB(21, gIdleSeq, 0, 0);                      // the game's frame starts
        reason = TOldRt::Call1((TAny*)c->iFn, (TInt)c->iPtr, result);
        CRUMB(22, gIdleSeq, reason, 0);                 // the game's frame returned (a 21 without a 22 = it died inside)
        }
    if (gAfterCallHook)
        {
        gAfterCallHook();               // may leave: the old frames are gone, only v3 frames are left
        }
    if (reason != KErrNone)
        {
        if (gLog && gIdleLogs < 4)
            {
            gIdleLogs++;
            gLog->Logf(_L8("idle: callback left with %d, idling stopped"), reason);
            }
        return 0;                           // do not run again
        }
    return result;                          // non-zero = run again, as in the old CIdle
    }

// Game classes that derive from CIdle: the object belongs to the game, the real v3 CIdle is kept here.
const TInt KMaxDerivedIdle = 8;

struct TDerivedIdle
    {
    TUint32 iObj;                           // address of the game's object (0 = free slot)
    CIdle* iReal;
    TIdleCtx* iCtx;                         // callback context of the last Start
    };

static TDerivedIdle gDerived[KMaxDerivedIdle];

static TDerivedIdle* FindDerived(TAny* aObj)        // call with the lock held
    {
    for (TInt i = 0; i < KMaxDerivedIdle; i++)
        {
        if (gDerived[i].iObj == (TUint32)aObj && aObj)
            {
            return &gDerived[i];
            }
        }
    return NULL;
    }

static CIdle* RealIdle(TAny* aFake)
    {
    TUint32* o = (TUint32*)aFake;
    if (o && o[0] == (TUint32)gIdleVt)
        {
        return (CIdle*)o[1];
        }
    Lock();
    TDerivedIdle* d = FindDerived(aFake);
    CIdle* real = d ? d->iReal : NULL;
    Unlock();
    return real;
    }

static void IdleFakeDtor(TAny* aThis, TInt aFlags)
    {
    TUint32* o = (TUint32*)aThis;
    CIdle* real = (CIdle*)o[1];

    if (real)
        {
        real->Cancel();
        delete real;
        o[1] = 0;
        }
    User::Free((TAny*)o[2]);                // callback context
    o[2] = 0;
    if (aFlags & 1)
        {
        User::Free(aThis);
        }
    }

// euser 748: static CIdle* CIdle::New(TInt aPriority)
static TAny* ShimIdleNew(TInt aPriority)
    {
    CIdle* real = CIdle::New(aPriority);
    if (real && !real->IsAdded())           // defensive: same rule as for CTimer
        {
        CActiveScheduler::Add(real);
        }
    TUint32* o = NULL;
    if (real)
        {
        o = (TUint32*)User::AllocZ(KFakeIdleSize);
        if (o)
            {
            o[0] = (TUint32)gIdleVt;
            o[1] = (TUint32)real;
            }
        else
            {
            delete real;
            }
        }
    if (gLog)
        {
        gLog->Logf(_L8("idle: CIdle::New(priority %d) -> fake %08x"), aPriority, (TUint32)o);
        }
    return o;
    }

// euser 728: static CIdle* CIdle::NewL(TInt aPriority) -- like New, but leaves when out of memory
static TAny* ShimIdleNewL(TInt aPriority)
    {
    TAny* o = ShimIdleNew(aPriority);
    if (!o)
        {
        TOldRt::Leave(KErrNoMemory);
        }
    return o;
    }

// euser 1108: void CIdle::Start(TCallBack)   -- the 8-byte TCallBack arrives in r1:r2
static void ShimIdleStart(TAny* aThis, TInt (*aFn)(TAny*), TAny* aPtr)
    {
    CIdle* real = RealIdle(aThis);
    if (!real)
        {
        if (gLog)
            {
            gLog->Logf(_L8("idle: Start on %08x which is not a fake CIdle, ignored"), (TUint32)aThis);
            }
        return;
        }
    TUint32* o = (TUint32*)aThis;
    TIdleCtx* ctx = (TIdleCtx*)User::AllocZ(sizeof(TIdleCtx));
    if (!ctx)
        {
        return;
        }
    ctx->iFn = aFn;
    ctx->iPtr = aPtr;
    real->Cancel();
    Lock();
    TDerivedIdle* d = FindDerived(aThis);
    TIdleCtx* oldCtx = NULL;
    if (d)
        {
        oldCtx = d->iCtx;
        d->iCtx = ctx;
        }
    else
        {
        oldCtx = (TIdleCtx*)o[2];
        o[2] = (TUint32)ctx;
        }
    Unlock();
    User::Free(oldCtx);

    real->Start(TCallBack(IdleThunk, ctx));
    if (gLog)
        {
        gLog->Logf(_L8("idle: Start(fn=%08x, ptr=%08x)"), (TUint32)aFn, (TUint32)aPtr);
        }
    }

// euser 1360: CIdle::CIdle(TInt aPriority), the base constructor called from the game's own constructor.
// aThis is the game's object; the real v3 CIdle is created and remembered in gDerived. CIdle::New adds it
// to the scheduler of the creating thread, so CActiveScheduler::Add (below) has nothing left to do.
static TAny* ShimIdleCtor(TAny* aThis, TInt aPriority)
    {
    CIdle* real = CIdle::New(aPriority);
    if (real && !real->IsAdded())
        {
        CActiveScheduler::Add(real);
        }
    TBool stored = EFalse;
    if (real)
        {
        Lock();
        for (TInt i = 0; i < KMaxDerivedIdle && !stored; i++)
            {
            if (gDerived[i].iObj == 0 || gDerived[i].iObj == (TUint32)aThis)
                {
                gDerived[i].iObj = (TUint32)aThis;
                gDerived[i].iReal = real;
                gDerived[i].iCtx = NULL;
                stored = ETrue;
                }
            }
        Unlock();
        if (!stored)
            {
            delete real;
            }
        }
    if (gLog)
        {
        gLog->Logf(_L8("idle: CIdle::CIdle(priority %d) on game object %08x -> real %08x%s"),
                   aPriority, (TUint32)aThis, (TUint32)real, stored ? "" : "  (FAILED, Start will be ignored)");
        }
    return aThis;
    }

// euser 13: static void CActiveScheduler::Add(CActive*). Only CIdle-derived objects are known so far; the real
// CIdle is already added (see above). Any other active object is reported once and ignored.
static TInt gAddLogs = 0;
static void ShimSchedAdd(TAny* aActive)
    {
    Lock();
    TDerivedIdle* d = FindDerived(aActive);
    Unlock();
    if (!d && ActiveShim_Add(aActive))
            {
            return;                         // a derived CActive, handled in activeshim.cpp
            }
    if (!d && gLog && gAddLogs < 4)
        {
        gAddLogs++;
        gLog->Logf(_L8("sched: CActiveScheduler::Add(%08x) on an unknown old active object, ignored"), (TUint32)aActive);
        }
    }

// euser 1263: CIdle::~CIdle(), reached through word 2 of the game's vtable (deleting call, flags & 1 = free)
// or as the base destructor of a derived destructor.
static void ShimIdleDtor(TAny* aThis, TInt aFlags)
    {
    CIdle* real = NULL;
    TIdleCtx* ctx = NULL;
    Lock();
    TDerivedIdle* d = FindDerived(aThis);
    if (d)
        {
        real = d->iReal;
        ctx = d->iCtx;
        d->iObj = 0;
        d->iReal = NULL;
        d->iCtx = NULL;
        }
    Unlock();
    if (real)
        {
        real->Cancel();
        delete real;
        }
    User::Free(ctx);
    if (aFlags & 1)
        {
        User::Free(aThis);
        }
    }

// euser 152: void CActive::Cancel() -- fake CIdle / derived CIdle / fake CPeriodic objects
static void ShimActiveCancel(TAny* aThis)
    {
    CIdle* real = RealIdle(aThis);
    if (real)
        {
        real->Cancel();
        return;
        }
    TUint32* o = (TUint32*)aThis;
    if (o && o[0] == (TUint32)gPeriodicVt && o[1])
        {
        ((CPeriodic*)o[1])->Cancel();
        }
    ActiveShim_Cancel(aThis);
    }

// ---------------------------------------------------------------------------
void SchedShim_SetAfterCallHook(void (*aHook)())
    {
    gAfterCallHook = aHook;
    }

void SchedShim_Init(THostLog* aLog)
    {
    gLog = aLog;
    if (!gLockReady && gLock.CreateLocal() == KErrNone)
        {
        gLockReady = ETrue;
        }
    gSchedVt[2] = (TUint32)&SchedFakeDtor;
    gIdleVt[2] = (TUint32)&IdleFakeDtor;

    gPeriodicVt[2] = (TUint32)&PeriodicFakeDtor;
    for (TInt i = 3; i < 8; i++)
        {
        gPeriodicVt[i] = (TUint32)&FakeSlot;
        gSchedVt[i] = (TUint32)&FakeSlot;
        gIdleVt[i] = (TUint32)&FakeSlot;
        }
    }

void SchedShim_SetMaxFps(TInt aFps)
    {
    gMinIntervalUs = (aFps > 0) ? (1000000 / aFps) : 0;
    gLastRunUs = 0;
    }

// euser 21: User::After(TTimeIntervalMicroSeconds32). The plain After rounds up to the 15.6 ms
// timer tick, which makes games that pace their frames with it jitter. 1 ms resolution instead.
static void ShimAfter(TInt aMicroSeconds)
    {
    if (aMicroSeconds > 0)
        {
        User::AfterHighRes(TTimeIntervalMicroSeconds32(aMicroSeconds));
        }
    }

static const TShimRegEntry KSchedShimEntries[] =
    {
    { "euser", 21, (TUint32)&ShimAfter, 0, "schedshim", NULL },
    { "euser", 1343, (TUint32)&ShimSchedCtor, 0, "schedshim", NULL },
    { "euser", 569,  (TUint32)&ShimSchedInstall, 0, "schedshim", NULL },
    { "euser", 1107, (TUint32)&ShimSchedStart, 0, "schedshim", NULL },
    { "euser", 1113, (TUint32)&ShimSchedStop, 0, "schedshim", NULL },
    { "euser", 748,  (TUint32)&ShimIdleNew, 0, "schedshim", NULL },
    { "euser", 728,  (TUint32)&ShimIdleNewL, 0, "schedshim", NULL },
    { "euser", 1108, (TUint32)&ShimIdleStart, 0, "schedshim", NULL },
    { "euser", 152,  (TUint32)&ShimActiveCancel, 0, "schedshim", NULL },
    { "euser", 1360, (TUint32)&ShimIdleCtor, 0, "schedshim", NULL },
    { "euser", 13,   (TUint32)&ShimSchedAdd, 0, "schedshim", NULL },
    { "euser", 1263, (TUint32)&ShimIdleDtor, 0, "schedshim", NULL },
    { "euser", 752,  (TUint32)&ShimPeriodicNew, 0, "schedshim", NULL },
	{ "euser", 738,  (TUint32)&ShimPeriodicNewL, 0, "schedshim", NULL },
    { "euser", 1259, (TUint32)&SchedFakeDtor, 0, "schedshim", NULL },
    { "euser", 1111, (TUint32)&ShimPeriodicStart, 0, "schedshim", NULL }
    };

TInt SchedShimReg_Init()
    {
    return ShimReg_Add(KSchedShimEntries, sizeof(KSchedShimEntries) / sizeof(KSchedShimEntries[0]));
    }
