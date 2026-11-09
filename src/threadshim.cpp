// Threads, semaphores, mutexes, exceptions-as-signals and TLS of the old game on top of the real v3 API.
// A game thread is a real v3 thread whose body runs old code through TOldRt (see oldrt.h). Every game thread
// is watched by a small helper thread that logs how it ended.
#include <e32base.h>
#include <e32std.h>
#include "threadshim.h"
#include "oldrt.h"
#include "shimreg.h"

static THostLog* gLog = NULL;
static RFastLock gLock;
static TBool gLockReady = EFalse;

static TInt gMinStack = 0x10000;                    // profile: thread_stack_min (old frames up to 1.8 KB + ~550 byte TParse in shims)
const TInt KMaxSlots = 8;
const TInt KUiWaitCapMicros = 10 * 1000 * 1000;     // bound for semaphore waits on non-game threads
static TThreadPriority gHighPrio = EPriorityNormal; // profile: thread_high_priority (the game loop may spin: never above the UI thread)

// Exit trick, from the profile (stop_state_*). Offsets in bytes; gStopTls == 0 = trick off.
static TInt gStopTls = 0;
static TInt gStopField = 0;
static TInt gStopValue = 0;

// Pending thread contexts: Create .. (SetInitialParameter) .. first run.
struct TThreadCtx
    {
    TUint32 iKey;                                   // address of the OLD RThread object
    TThreadFunction iFn;
    TAny* iParam;
    };

static TThreadCtx* gCtx[KMaxSlots];
static TUint64 gLive[KMaxSlots];                    // ids of running game threads

const TInt KSigQueue = 8;
class CSignalAO;

struct TSigRec
    {
    TUint64 iThread;                    // 0 = free
    TUint32 iHandler;                   // old void handler(TExcType), 0 until installed
    TUint32 iMask;
    CSignalAO* iAo;
    TInt iQ[KSigQueue];
    TInt iQCount;
    TBool iArmed;                       // iStatus is pending and the AO is active
    };

static TSigRec gSig[KMaxSlots];

static TInt gNameSeq = 0;

static void Lock()   { if (gLockReady) { gLock.Wait(); } }
static void Unlock() { if (gLockReady) { gLock.Signal(); } }

// A default-constructed old RThread holds KCurrentThreadHandle (same value in v2 and v3). If Create
// failed (old code ignores the result) or was never called, Resume/SetPriority would silently act
// on the CURRENT thread, which is the UI thread. Refuse in that case.
static TBool IsCreatedThread(const RThread* aThis, const char* aWhat)
    {
    const TInt h = aThis->Handle();
    if (h == 0 || h == KCurrentThreadHandle)
        {
        if (gLog)
            {
            TPtrC8 w((const TUint8*)aWhat);
            gLog->Logf(_L8("thread: %S ignored: the RThread was never created (handle %08x)"), &w, h);
            }
        return EFalse;
        }
    return ETrue;
    }

static void Log(const char* aText)
    {
    if (gLog)
        {
        TPtrC8 p((const TUint8*)aText);
        gLog->Text(p);
        }
    }

// aHighPriorityAs: -1 less, 0 normal, 1 more = what the old "realtime / high" requests become.
void ThreadShim_SetPolicy(TInt aMinStack, TInt aHighPriorityAs)
    {
    gMinStack = aMinStack;
    gHighPrio = aHighPriorityAs < 0 ? EPriorityLess : (aHighPriorityAs > 0 ? EPriorityMore : EPriorityNormal);
    }

void ThreadShim_SetStopState(TInt aTlsOffset, TInt aFieldOffset, TInt aValue)
    {
    gStopTls = aTlsOffset;
    gStopField = aFieldOffset;
    gStopValue = aValue;
    }

void ThreadShim_Init(THostLog* aLog)
    {
    gLog = aLog;
    if (!gLockReady && gLock.CreateLocal() == KErrNone)
        {
        gLockReady = ETrue;
        }
    }

// ---------------------------------------------------------------------------
// context table
// ---------------------------------------------------------------------------
static TInt AddCtx(TThreadCtx* aCtx)
    {
    TInt r = KErrOverflow;
    Lock();
    for (TInt i = 0; i < KMaxSlots && r != KErrNone; i++)
        {
        if (!gCtx[i])
            {
            gCtx[i] = aCtx;
            r = KErrNone;
            }
        }
    Unlock();
    return r;
    }

static void RemoveCtx(TThreadCtx* aCtx)             // unlink and free
    {
    Lock();
    for (TInt i = 0; i < KMaxSlots; i++)
        {
        if (gCtx[i] == aCtx)
            {
            gCtx[i] = NULL;
            }
        }
    Unlock();
    User::Free(aCtx);
    }

static TInt SetParamFor(TUint32 aKey, TAny* aParam)
    {
    TInt r = KErrNotFound;
    Lock();
    for (TInt i = 0; i < KMaxSlots; i++)
        {
        if (gCtx[i] && gCtx[i]->iKey == aKey)
            {
            gCtx[i]->iParam = aParam;
            r = KErrNone;
            }
        }
    Unlock();
    return r;
    }

static void ClearLive(TUint64 aId)
    {
    Lock();
    for (TInt i = 0; i < KMaxSlots; i++)
        {
        if (gSig[i].iThread == aId)
            {
            gSig[i].iThread = 0;            // the AO died with its thread
            }
        }
    for (TInt i = 0; i < KMaxSlots; i++)
        {
        if (gLive[i] == aId)
            {
            gLive[i] = 0;
            }
        }
    Unlock();
    }

// ETrue while at least one game thread is really running (asks the kernel, not the bookkeeping).
static TBool AnyGameThreadAlive()
    {
    for (TInt i = 0; i < KMaxSlots; i++)
        {
        TUint64 id = 0;
        Lock();
        id = gLive[i];
        Unlock();
        if (id == 0)
            {
            continue;
            }
        RThread t;
        if (t.Open(TThreadId(id)) != KErrNone)
            {
            continue;
            }
        const TBool alive = (t.ExitType() == EExitPending);
        t.Close();
        if (alive)
            {
            return ETrue;
            }
        }
    return EFalse;
    }

static void MarkLive(TBool aOn)
    {
    RThread me;
    const TUint64 id = me.Id().Id();
    Lock();
    for (TInt i = 0; i < KMaxSlots; i++)
        {
        if (aOn && gLive[i] == 0)
            {
            gLive[i] = id;
            break;
            }
        if (!aOn && gLive[i] == id)
            {
            gLive[i] = 0;
            break;
            }
        }
    Unlock();
    }

// ---------------------------------------------------------------------------
// death watcher: one small thread per game thread. It waits for the game thread to end and logs
// how: exit type / reason / category, and the last trap or trace stub the thread hit. A panic of
// the game thread is otherwise invisible (only the UI thread is watched by CRASHWATCH).
// ---------------------------------------------------------------------------
struct TWatchArg
    {
    TUint64 iId;
    };

static TInt WatcherEntry(TAny* aArg)
    {
    const TUint64 id = ((TWatchArg*)aArg)->iId;
    User::Free(aArg);
    RThread t;
    if (t.Open(TThreadId(id)) != KErrNone)
        {
        ClearLive(id);
        return KErrNotFound;
        }
    TRequestStatus st;
    t.Logon(st);
    User::WaitForRequest(st);
    const TExitType type = t.ExitType();
    const TInt reason = t.ExitReason();
    TBuf8<32> cat;
    {
    const TExitCategoryName c = t.ExitCategory();
    cat.Copy(c.Left(cat.MaxLength()));
    }
    t.Close();
    TInt hit = -1;
    TInt depth = 0;
    const TInt info = TOldRt::ThreadInfo(id, hit, depth);
    ClearLive(id);
    if (gLog)
        {
        if (info == KErrNone)
            {
            gLog->Logf(_L8("thread: game thread %x ended: type=%d (0=kill 1=terminate 2=panic) reason=%d category='%S' lasthit=#%d cleanupdepth=%d"),
                       (TUint32)id, (TInt)type, reason, &cat, hit, depth);
            }
        else
            {
            gLog->Logf(_L8("thread: game thread %x ended: type=%d (0=kill 1=terminate 2=panic) reason=%d category='%S' (state already detached: it returned normally)"),
                       (TUint32)id, (TInt)type, reason, &cat);
            }
        }
    return KErrNone;
    }

static void StartWatcher(const RThread& aGame, TInt aSeq)
    {
    TWatchArg* a = (TWatchArg*)User::AllocZ(sizeof(TWatchArg));
    if (!a)
        {
        return;
        }
    a->iId = aGame.Id().Id();
    TName wn;
    wn.Copy(_L("gwatch_"));
    wn.AppendNumFixedWidth(aSeq, EHex, 4);
    RThread w;
    const TInt r = w.Create(wn, WatcherEntry, 0x4000, (RAllocator*)NULL, a, EOwnerProcess);
    if (r == KErrNone)
        {
        w.Resume();
        w.Close();
        }
    else
        {
        User::Free(a);
        if (gLog)
            {
            gLog->Logf(_L8("thread: watcher Create -> %d"), r);
            }
        }
    }

// ---------------------------------------------------------------------------
// the trampoline: runs on the NEW thread
// ---------------------------------------------------------------------------
static TInt ThreadEntry(TAny* aCtx)
    {
    TThreadCtx* ctx = (TThreadCtx*)aCtx;
    TThreadFunction fn = ctx->iFn;
    TAny* param = ctx->iParam;
    RemoveCtx(ctx);

    CTrapCleanup* cleanup = CTrapCleanup::New();    // 1. cleanup stack
    if (!cleanup)
        {
        return KErrNoMemory;
        }
    TInt result = KErrNone;
    TInt err = TOldRt::AttachThread();              // 2. per-thread old runtime state
    if (err == KErrNone)
        {
        TOldRt::State().iIsGameThread = ETrue;
        MarkLive(ETrue);
        if (gLog)
            {
            gLog->Logf(_L8("thread: game thread running fn=%08x param=%08x"), (TUint32)fn, (TUint32)param);
            }
        TInt oldResult = 0;
        TInt reason = KErrNone;
        TRAP(err, reason = TOldRt::Call1((TAny*)fn, (TInt)param, oldResult));   // 3. old code
        if (gLog)
            {
            gLog->Logf(_L8("thread: game thread finished: trap=%d old-leave=%d result=%d lasthit=%d"),
                       err, reason, oldResult, TOldRt::State().iHit);
            }
        result = (err != KErrNone) ? err : reason;
        MarkLive(EFalse);
        TOldRt::DetachThread();                     // 4. state gone before the cleanup stack
        }
    delete cleanup;
    return result;
    }

// ---------------------------------------------------------------------------
// old RThread API
// ---------------------------------------------------------------------------
// euser 289: TInt RThread::Create(const TDesC&, TThreadFunction, TInt aStack, RHeap* aHeap,
//                                 TAny* aPtr, TOwnerType)  -- r0..r3 + 3 words on the stack
static TInt ShimThreadCreate(RThread* aThis, const TDesC* aName, TThreadFunction aFn,
                             TInt aStack, TAny* aHeap, TAny* aPtr, TInt aType)
    {
    TThreadCtx* ctx = (TThreadCtx*)User::AllocZ(sizeof(TThreadCtx));
    if (!ctx)
        {
        return KErrNoMemory;
        }
    ctx->iKey = (TUint32)aThis;
    ctx->iFn = aFn;
    ctx->iParam = aPtr;
    TInt err = AddCtx(ctx);
    if (err != KErrNone)
        {
        User::Free(ctx);
        return err;
        }
    const TInt stack = (aStack < gMinStack) ? gMinStack : aStack;
    TInt seq;
    Lock();
    seq = ++gNameSeq;
    Unlock();
    TName name;                                     // v3 thread names must be unique
    const TInt nameLen = aName->Length();
    if (nameLen > 0 && nameLen <= 48)
        {
        name.Copy(*aName);
        }
    else
        {
        name.Copy(_L("gamethread"));
        Log("thread: old thread name looks invalid, using a default");
        }
    name.Append('_');
    name.AppendNumFixedWidth(seq, EHex, 4);
    // NULL heap: the thread shares the creator's heap (see header comment).
    // KErrTooBig (-40) came back for the old stack size of 0x20000, so retry with smaller stacks,
    // never below gMinStack. Every attempt is logged.
    TInt tryStack = stack;
    TBuf8<96> n;
    n.Copy(name.Left(n.MaxLength()));
    for (;;)
        {
        err = aThis->Create(name, ThreadEntry, tryStack, (RAllocator*)NULL, ctx, (TOwnerType)aType);
        if (gLog)
            {
            gLog->Logf(_L8("thread: Create('%S', fn=%08x, stack %x (old %x), old heap %08x, ptr %08x, type %d) -> %d"),
                       &n, (TUint32)aFn, tryStack, aStack, (TUint32)aHeap, (TUint32)aPtr, aType, err);
            }
        if (err != KErrTooBig || tryStack <= gMinStack)
            {
            break;
            }
        tryStack = (tryStack > 0x14000) ? 0x14000 : gMinStack;
        }
    if (err == KErrNone)
        {
        StartWatcher(*aThis, seq);          // before Resume: Logon is in place when the thread starts
        }
    else
        {
        RemoveCtx(ctx);
        }
    return err;
    }

// euser 992: TInt RThread::SetInitialParameter(TAny*)  (removed in v3)
static TInt ShimThreadSetParam(RThread* aThis, TAny* aParam)
    {
    const TInt r = SetParamFor((TUint32)aThis, aParam);
    if (gLog)
        {
        gLog->Logf(_L8("thread: SetInitialParameter(%08x) -> %d"), (TUint32)aParam, r);
        }
    return r;
    }

// euser 1016: void RThread::SetPriority(TThreadPriority)  -- the enumerator VALUES changed in v3
static void ShimThreadSetPriority(RThread* aThis, TInt aOld)
    {
    TThreadPriority p = EPriorityNormal;
    TBool clamp = EFalse;
    switch (aOld)
        {
        case -30: p = EPriorityNull;                break;
        case -20: p = EPriorityMuchLess;            break;
        case -10: p = EPriorityLess;                break;
        case 0:   p = EPriorityNormal;              break;
        case 10:  p = EPriorityMore;      clamp = ETrue; break;
        case 20:  p = EPriorityMuchMore;  clamp = ETrue; break;
        case 30:  p = EPriorityRealTime;  clamp = ETrue; break;
        case 100: p = EPriorityAbsoluteVeryLow;     break;
        case 200: p = EPriorityAbsoluteLow;         break;
        case 300: p = EPriorityAbsoluteBackground;  break;
        case 400: p = EPriorityAbsoluteForeground;  break;
        case 500: p = EPriorityAbsoluteHigh; clamp = ETrue; break;
        default:  break;
        }
    if (clamp)
        {
        p = gHighPrio;
        }
    if (!IsCreatedThread(aThis, "SetPriority"))
        {
        return;
        }
    aThis->SetPriority(p);
    if (gLog)
        {
        gLog->Logf(_L8("thread: SetPriority(old %d), clamped=%d"), aOld, clamp);
        }
    }

// euser 954: void RThread::Resume() const
static void ShimThreadResume(RThread* aThis)
    {
    if (!IsCreatedThread(aThis, "Resume"))
        {
        return;
        }
    aThis->Resume();
    Log("thread: Resume");
    }

// ---------------------------------------------------------------------------
// RThread::RaiseException / SetExceptionHandler (removed in v3) -- emulated.
// The protocol (seen in the log of the first game that needed it): the game thread sits in its active scheduler with NO active objects;
// the UI thread stores a command, raises EExcUserInterrupt (22) on the game thread and waits; the
// handler the game thread installed with SetExceptionHandler runs the engine step (it reads the
// engine from TLS) in the game thread. Emulation: the game thread owns a real active object
// (CSignalAO, high priority) whose request the UI thread completes with RThread::RequestComplete;
// RunL calls the old handler through TOldRt::Call1 inside the game thread. Raises that arrive
// before the handler exists are queued and delivered once it is installed.
// ---------------------------------------------------------------------------
static TSigRec* SigFind(TUint64 aId, TBool aCreate)     // call with the lock held
    {
    for (TInt i = 0; i < KMaxSlots; i++)
        {
        if (gSig[i].iThread == aId)
            {
            return &gSig[i];
            }
        }
    if (aCreate)
        {
        for (TInt i = 0; i < KMaxSlots; i++)
            {
            if (gSig[i].iThread == 0)
                {
                Mem::FillZ(&gSig[i], sizeof(TSigRec));
                gSig[i].iThread = aId;
                return &gSig[i];
                }
            }
        }
    return NULL;
    }

static TUint64 MyThreadId()
    {
    RThread me;
    return me.Id().Id();
    }

class CSignalAO : public CActive
    {
public:
    CSignalAO(TUint64 aId) : CActive(EPriorityHigh), iId(aId) { CActiveScheduler::Add(this); }
    ~CSignalAO() { Cancel(); }
    void Start();                       // game thread: arm (and fire at once if raises are queued)
    void DoCancel();
    void RunL();
private:
    TUint64 iId;
    };

void CSignalAO::Start()
    {
    Lock();
    TSigRec* r = SigFind(iId, EFalse);
    iStatus = KRequestPending;
    SetActive();
    if (r)
        {
        r->iArmed = ETrue;
        if (r->iQCount > 0)
            {
            r->iArmed = EFalse;
            RThread me;
            TRequestStatus* st = &iStatus;
            me.RequestComplete(st, KErrNone);
            }
        }
    Unlock();
    }

void CSignalAO::DoCancel()
    {
    Lock();
    TSigRec* r = SigFind(iId, EFalse);
    if (r)
        {
        r->iArmed = EFalse;
        }
    const TBool pending = (iStatus.Int() == KRequestPending);
    Unlock();
    if (pending)
        {
        RThread me;
        TRequestStatus* st = &iStatus;
        me.RequestComplete(st, KErrCancel);
        }
    }

void CSignalAO::RunL()
    {
    for (;;)
        {
        Lock();
        TSigRec* r = SigFind(iId, EFalse);
        if (!r)
            {
            Unlock();
            return;
            }
        if (r->iQCount == 0)            // nothing left: re-arm under the lock
            {
            iStatus = KRequestPending;
            SetActive();
            r->iArmed = ETrue;
            Unlock();
            return;
            }
        const TInt type = r->iQ[0];
        for (TInt i = 1; i < r->iQCount; i++)
            {
            r->iQ[i - 1] = r->iQ[i];
            }
        r->iQCount--;
        const TUint32 handler = r->iHandler;
        const TUint32 mask = r->iMask;
        Unlock();
        if (handler != 0 && type >= 0 && type < 32 && (mask & (1u << type)))
            {
            TInt res = 0;
            const TInt reason = TOldRt::Call1((TAny*)handler, type, res);
            if (gLog && reason != KErrNone)
                {
                gLog->Logf(_L8("signal: old handler(%d) left with %d"), type, reason);
                }
            }
        }
    }

// euser 985: TInt RThread::SetExceptionHandler(void (*)(TExcType), TUint32 aMask)
static TInt ShimSetExceptionHandler(RThread* aThis, TAny* aHandler, TUint32 aMask)
    {
    const TUint64 id = aThis->Id().Id();
    const TBool mine = (id == MyThreadId());
    TBool needAo = EFalse;
    Lock();
    TSigRec* r = SigFind(id, ETrue);
    if (r)
        {
        r->iHandler = (TUint32)aHandler;
        r->iMask = aMask;
        needAo = (r->iAo == NULL);
        }
    Unlock();
    TInt state = 0;                     // 0 = recorded only, 1 = active object armed
    if (r && needAo && mine)
        {
        if (CActiveScheduler::Current())
            {
            CSignalAO* ao = new CSignalAO(id);
            if (ao)
                {
                Lock();
                r = SigFind(id, EFalse);
                if (r)
                    {
                    r->iAo = ao;
                    }
                Unlock();
                ao->Start();
                state = 1;
                }
            }
        else
            {
            Log("signal: SetExceptionHandler without an installed scheduler, not armed");
            }
        }
	static TInt sigLogs = 0;                // the game calls this every frame: log only the first few
    if (gLog && sigLogs < 8)
        {
        sigLogs++;
        gLog->Logf(_L8("signal: SetExceptionHandler(handler=%08x, mask=%08x) thread %x, armed=%d"),
                   (TUint32)aHandler, aMask, (TUint32)id, state);
        }
    return KErrNone;
    }

// euser 868: TInt RThread::RaiseException(TExcType)
static TInt ShimRaiseException(RThread* aThis, TInt aType)
    {
    const TUint64 id = aThis->Id().Id();
    CSignalAO* ao = NULL;
    TBool queued = EFalse;
    TBool wake = EFalse;
    Lock();
    TSigRec* r = SigFind(id, ETrue);
    if (r)
        {
        if (r->iQCount < KSigQueue)
            {
            r->iQ[r->iQCount++] = aType;
            queued = ETrue;
            }
        if (r->iArmed && r->iAo)
            {
            r->iArmed = EFalse;
            ao = r->iAo;
            wake = ETrue;
            }
        }
    Unlock();
    if (wake)
        {
        RThread t;
        if (t.Open(TThreadId(id)) == KErrNone)
            {
            TRequestStatus* st = &ao->iStatus;
            t.RequestComplete(st, KErrNone);
            t.Close();
            }
        }
    return KErrNone;
    }

// euser 1137: static TUint User::TickCount(). The old tick is 1/64 s; the v3 one has another period,
// and the game's handler throttles itself on "ticks since last step > 8".
static TUint ShimTickCount()
    {
    return (TUint)(((TUint64)User::NTickCount() * 64) / 1000);
    }

// ---------------------------------------------------------------------------
// semaphores and mutexes (one handle word in both SDKs)
// ---------------------------------------------------------------------------
static TInt ShimSemCreateLocal(RSemaphore* aThis, TInt aCount, TInt aType)      // euser 280
    {
    return aThis->CreateLocal(aCount, (TOwnerType)aType);
    }

static void ShimSemWait(RSemaphore* aThis)                                       // euser 1212
    {
    if (TOldRt::State().iIsGameThread)
        {
        aThis->Wait();
        return;
        }
    // UI thread: wait in short slices, stop early when no game thread is alive any more (a thread
    // that died or never started cannot signal), and never longer than KUiWaitCapMicros in total.
    const TInt slice = 200 * 1000;
    TInt waited = 0;
    while (aThis->Wait(slice) == KErrTimedOut)
        {
        waited += slice;
        if (!AnyGameThreadAlive())
            {
            Log("thread: RSemaphore::Wait abandoned: no game thread is alive");
            return;
            }
        if (waited >= KUiWaitCapMicros)
            {
            Log("thread: RSemaphore::Wait on a non-game thread timed out (10 s), continuing");
            return;
            }
        }
    }

static void ShimSemSignal(RSemaphore* aThis)                                     // euser 1078
    {
    aThis->Signal();
    }

static TInt ShimMutexCreateLocal(RMutex* aThis, TInt aType)                      // euser 283
    {
    return aThis->CreateLocal((TOwnerType)aType);
    }

static void ShimMutexWait(RMutex* aThis)                                         // euser 1214
    {
    aThis->Wait();
    }

static void ShimMutexSignal(RMutex* aThis)                                       // euser 1081
    {
    aThis->Signal();
    }

// ---------------------------------------------------------------------------
// Dll::SetTls / Tls / FreeTls of the old game (UserSvr::DllSetTls etc.). The game stores its engine
// object there and finds it again from anywhere in the thread. The v3 UserSvr functions have a
// different signature, so TLS is emulated per thread in TOldThreadState (one module = one slot).
// ---------------------------------------------------------------------------
static TInt gTlsLogs = 0;
static TAny* gGameTls = NULL;                       // the game's Dll::Tls pointer (for the exit trick)

static TInt ShimDllSetTls(TInt aHandle, TAny* aPtr)                              // euser 335
    {
    TOldRt::State().iTls = aPtr;
    if (aPtr)
        {
        gGameTls = aPtr;
        }
    if (gLog && gTlsLogs < 6)
        {
        gTlsLogs++;
        gLog->Logf(_L8("tls: set handle=%08x ptr=%08x"), aHandle, (TUint32)aPtr);
        }
    return KErrNone;
    }

static TAny* ShimDllTls(TInt /*aHandle*/)                                        // euser 336
    {
    return TOldRt::State().iTls;
    }

static void ShimDllFreeTls(TInt /*aHandle*/)                                     // euser 328
    {
    TOldRt::State().iTls = NULL;
    }

// euser 746: CTrapCleanup* CTrapCleanup::New(). The trampoline already installed the real v3 cleanup
// stack of this thread; a second one would panic. The old code gets a fake object whose old-style
// (GCC 2.9) vtable has a no-op destructor at word 2, as for the other fake objects.
static TUint32 gCleanupVt[8];
static TUint32 gCleanupObj[8][2];
static TInt gCleanupNext = 0;

static void CleanupFakeDtor(TAny*, TInt) {}

static TInt CleanupFakeSlot()
    {
    Log("cleanup: unexpected virtual call on the fake CTrapCleanup (returns 0)");
    return 0;
    }

static TAny* ShimCTrapCleanupNew()
    {
    if (gCleanupVt[2] == 0)
        {
        gCleanupVt[2] = (TUint32)&CleanupFakeDtor;
        for (TInt i = 3; i < 8; i++)
            {
            gCleanupVt[i] = (TUint32)&CleanupFakeSlot;
            }
        }
    Lock();
    const TInt n = gCleanupNext++ & 7;
    Unlock();
    gCleanupObj[n][0] = (TUint32)gCleanupVt;
    gCleanupObj[n][1] = 0;
    Log("cleanup: CTrapCleanup::New answered with a fake object");
    return gCleanupObj[n];
    }

// Exit trick: the game's control block, found through its Dll::Tls data (stop_state_tls in the profile is the
// byte offset of the block pointer inside the TLS data). NULL when the trick is off or the layout does not fit.
static TUint32* StopBlock()
    {
    if (gStopTls == 0 || !gGameTls)
        {
        return NULL;
        }
    const TUint32 obj = ((const TUint32*)gGameTls)[gStopTls / 4];
    if (!obj || (obj & 3))
        {
        return NULL;
        }
    return (TUint32*)obj;
    }

// Graceful stop. The game's signal handler acts at most once per 8 ticks (125 ms), so the stop
// signals from HandleLosingForeground can be ignored: signal again every KResignalMs while waiting.
// Returns ETrue when no game thread is running any more; ThreadShim_Shutdown kills what is left.
TBool ThreadShim_StopAndWait(TInt aMaxMs)
    {
    const TUint KResignalMs = 150;
    const TUint start = User::NTickCount();
    TUint lastSignal = start;

    // The trick: what the game's own exit does (its AppUi destructor, which the host never runs) is to put
    // the state word of its control block into the "stop" state; that makes the signal handler call
    // CActiveScheduler::Stop, after which the thread runs its own cleanup. Only a thread that is still
    // running needs the push, and only a game with the layout the profile describes (stop_state_tls):
    // the block is read and WRITTEN through the game's TLS. A game that already stopped itself must not
    // be touched.
    if (gStopTls != 0 && AnyGameThreadAlive())
        {
        TUint32* block = StopBlock();
        if (block)
            {
            TUint32* word = block + gStopField / 4;
            const TInt before = (TInt)*word;
            *word = (TUint32)gStopValue;
            if (gLog)
                {
                gLog->Logf(_L8("thread: exit trick applied (tls %08x, block %08x): state %d -> %d"),
                           (TUint32)gGameTls, (TUint32)block, before, (TInt)*word);
                }
            }
        else if (gLog)
            {
            gLog->Logf(_L8("thread: exit trick not applied: no usable control block (tls %08x)"), (TUint32)gGameTls);
            }
        }

    for (;;)
        {
        const TUint now = User::NTickCount();
        const TBool resignal = (now - lastSignal) >= KResignalMs;
        TInt alive = 0;
        for (TInt i = 0; i < KMaxSlots; i++)
            {
            TUint64 id = 0;
            Lock();
            id = gLive[i];
            Unlock();
            if (id == 0)
                {
                continue;
                }
            RThread t;
            if (t.Open(TThreadId(id)) != KErrNone)
                {
                continue;
                }
            if (t.ExitType() == EExitPending)
                {
                alive++;
                if (resignal)
                    {
                    ShimRaiseException(&t, 22);
                    }
                }
            t.Close();
            }
        if (resignal)
            {
            lastSignal = now;
            }
        if (alive == 0)
            {
            if (gLog)
                {
                gLog->Logf(_L8("thread: game thread(s) ended by themselves after %d ms"), (TInt)(User::NTickCount() - start));
                }
            return ETrue;
            }
        if ((TInt)(now - start) >= aMaxMs)
            {
            if (gLog)
                {
                gLog->Logf(_L8("thread: graceful stop timed out after %d ms, %d thread(s) still running"), aMaxMs, alive);
                }
            return EFalse;
            }
        User::After(10000);                                 // 10 ms
        }
    }

void ThreadShim_Shutdown()
    {
    for (TInt i = 0; i < KMaxSlots; i++)
        {
        TUint64 id = 0;
        Lock();
        id = gLive[i];
        gLive[i] = 0;
        Unlock();
        if (id == 0)
            {
            continue;
            }
        RThread t;
        if (t.Open(TThreadId(id)) == KErrNone)
            {
            if (t.ExitType() == EExitPending)
                {
                t.Kill(KErrAbort);                  // its state is freed by TOldRt::Shutdown
                Log("thread: game thread still running at exit, killed");
                }
            t.Close();
            }
        }
    for (TInt i = 0; i < KMaxSlots; i++)            // contexts of threads that were never resumed
        {
        TThreadCtx* c = NULL;
        Lock();
        c = gCtx[i];
        gCtx[i] = NULL;
        Unlock();
        User::Free(c);
        }
    }

static const TShimRegEntry KThreadShimEntries[] =
    {
    { "euser", 289,  (TUint32)&ShimThreadCreate, 0, "threadshim", NULL },
    { "euser", 992,  (TUint32)&ShimThreadSetParam, 0, "threadshim", NULL },
    { "euser", 1016, (TUint32)&ShimThreadSetPriority, 0, "threadshim", NULL },
    { "euser", 954,  (TUint32)&ShimThreadResume, 0, "threadshim", NULL },
    { "euser", 868,  (TUint32)&ShimRaiseException, 0, "threadshim", NULL },
    { "euser", 985,  (TUint32)&ShimSetExceptionHandler, 0, "threadshim", NULL },
    { "euser", 280,  (TUint32)&ShimSemCreateLocal, 0, "threadshim", NULL },
    { "euser", 1212, (TUint32)&ShimSemWait, 0, "threadshim", NULL },
    { "euser", 1078, (TUint32)&ShimSemSignal, 0, "threadshim", NULL },
    { "euser", 283,  (TUint32)&ShimMutexCreateLocal, 0, "threadshim", NULL },
    { "euser", 1214, (TUint32)&ShimMutexWait, 0, "threadshim", NULL },
    { "euser", 1081, (TUint32)&ShimMutexSignal, 0, "threadshim", NULL },
    { "euser", 1137, (TUint32)&ShimTickCount, 0, "threadshim", NULL },
    { "euser", 335,  (TUint32)&ShimDllSetTls, 0, "threadshim", NULL },
    { "euser", 336,  (TUint32)&ShimDllTls, 0, "threadshim", NULL },
    { "euser", 328,  (TUint32)&ShimDllFreeTls, 0, "threadshim", NULL },
    { "euser", 746,  (TUint32)&ShimCTrapCleanupNew, 0, "threadshim", NULL }
    };

TInt ThreadShimReg_Init()
    {
    return ShimReg_Add(KThreadShimEntries, sizeof(KThreadShimEntries) / sizeof(KThreadShimEntries[0]));
    }
