#include <e32base.h>
#include <e32std.h>
#include "activeshim.h"
#include "oldrt.h"
#include "crashwatch.h"
#include "shimreg.h"

// See activeshim.h for the design.

static THostLog* gLog = NULL;
static RFastLock gLock;
static TBool gLockReady = EFalse;

static void Lock()   { if (gLockReady) { gLock.Wait(); } }
static void Unlock() { if (gLockReady) { gLock.Signal(); } }

// Old CActive (GCC 2.95, EKA1): vptr(0), iStatus(4), iLink(8..19), iActive(20) = 24 bytes, the game's
// own members follow. The old inline code reads these fields directly, so they are kept zeroed.
const TInt KOldActiveSize = 24;

// Old-style vtable: two header words, then the slots. For CActive: 2 = destructor, 3 = DoCancel,
// 4 = RunL, 5 = RunError.
const TInt KSlotDoCancel = 3;
const TInt KSlotRunL = 4;
const TInt KSlotRunError = 5;

static TUint64 MyId()
    {
    RThread me;
    return me.Id().Id();
    }

static TUint32 OldSlot(TUint32 aObj, TInt aSlot)
    {
    const TUint32 vt = *(const TUint32*)aObj;
    if (vt == 0 || (vt & 3) != 0)
        {
        return 0;
        }
    return ((const TUint32*)vt)[aSlot];
    }

// ---------------------------------------------------------------------------
// the real active object
// ---------------------------------------------------------------------------
class CGameActive : public CActive
    {
public:
    CGameActive(TUint32 aOld, TInt aPriority) : CActive(aPriority), iOld(aOld), iOwner(MyId()) {}
    TUint32 Old() const { return iOld; }
    TUint64 Owner() const { return iOwner; }
    TRequestStatus* Status() { return &iStatus; }
    void Arm()                                  // the game's SetActive: the request is now pending
        {
        iStatus = KRequestPending;
        SetActive();
        }
protected:
    void DoCancel();
    void RunL();
    TInt RunError(TInt aError);
private:
    TUint32 iOld;
    TUint64 iOwner;
    };

static TInt gRunLogs = 0;
static TInt gCompleteLogs = 0;
static TInt gMiscLogs = 0;

static void Log(const char* aText, TUint32 aA = 0, TInt aB = 0)
    {
    if (gLog && gMiscLogs < 40)
        {
        gMiscLogs++;
        TPtrC8 t((const TUint8*)aText);
        gLog->Logf(_L8("active: %S (obj=%08x arg=%d)"), &t, aA, aB);
        }
    }

void CGameActive::RunL()
    {
    const TUint32 fn = OldSlot(iOld, KSlotRunL);
    if (!fn)
        {
        Log("RunL: the game object has no RunL slot", iOld);
        return;
        }
    if (gLog && gRunLogs < 8)
        {
        gRunLogs++;
        gLog->Logf(_L8("active: RunL obj=%08x status=%d fn=%08x"), iOld, iStatus.Int(), fn);
        }
    TInt result = 0;
    const TInt reason = TOldRt::Call1((TAny*)fn, (TInt)iOld, result);
    if (reason != KErrNone)
        {
        Log("old RunL left", iOld, reason);
        User::Leave(reason);                    // plain v3 frame: the scheduler calls RunError
        }
    }

TInt CGameActive::RunError(TInt aError)
    {
    const TUint32 fn = OldSlot(iOld, KSlotRunError);
    if (!fn)
        {
        return aError;
        }
    TInt result = aError;
    const TInt reason = TOldRt::CallArgs((TAny*)fn, (TInt)iOld, aError, 0, 0, result);
    if (reason != KErrNone)
        {
        return aError;
        }
    Log("RunError returned", iOld, result);
    return result;                              // non-zero = not handled (the scheduler's Error follows)
    }

void CGameActive::DoCancel()
    {
    const TUint32 fn = OldSlot(iOld, KSlotDoCancel);
    if (fn)
        {
        TInt result = 0;
        TOldRt::Call1((TAny*)fn, (TInt)iOld, result);
        }
    if (iStatus.Int() == KRequestPending)       // the game's DoCancel did not complete it: never hang Cancel()
        {
        Log("DoCancel did not complete the request, completing it", iOld);
        RThread me;
        TRequestStatus* s = &iStatus;
        me.RequestComplete(s, KErrCancel);
        }
    }

// ---------------------------------------------------------------------------
// side table: game object <-> real active object
// ---------------------------------------------------------------------------
const TInt KMaxActive = 16;

struct TActiveEntry
    {
    TUint32 iOld;                   // address of the game's object (0 = free slot)
    CGameActive* iReal;
    };

static TActiveEntry gAct[KMaxActive];

static CGameActive* FindReal(TUint32 aOld)          // call with the lock held
    {
    for (TInt i = 0; i < KMaxActive; i++)
        {
        if (gAct[i].iOld == aOld && aOld != 0)
            {
            return gAct[i].iReal;
            }
        }
    return NULL;
    }

static CGameActive* FindByStatus(TUint32 aStatus)   // call with the lock held; the old iStatus is at +4
    {
    for (TInt i = 0; i < KMaxActive; i++)
        {
        if (gAct[i].iOld != 0 && gAct[i].iOld + 4 == aStatus)
            {
            return gAct[i].iReal;
            }
        }
    return NULL;
    }

// ---------------------------------------------------------------------------
// shims
// ---------------------------------------------------------------------------
// euser 1395: CActive::CActive(TInt aPriority) -- the base constructor called by the game's own constructor
static TAny* ShimActiveCtor(TAny* aThis, TInt aPriority)
    {
    Mem::FillZ(((TUint8*)aThis) + 4, KOldActiveSize - 4);   // vptr stays: the game sets its own right after
    CGameActive* real = new CGameActive((TUint32)aThis, aPriority);
    TBool stored = EFalse;
    if (real)
        {
        Lock();
        for (TInt i = 0; i < KMaxActive && !stored; i++)
            {
            if (gAct[i].iOld == 0)
                {
                gAct[i].iOld = (TUint32)aThis;
                gAct[i].iReal = real;
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
        gLog->Logf(_L8("active: CActive::CActive(priority %d) on game object %08x -> real %08x%s"),
                   aPriority, (TUint32)aThis, (TUint32)real, stored ? "" : "  (FAILED, this object will not run)");
        }
    return aThis;
    }

// euser 13 (called from schedshim.cpp): CActiveScheduler::Add(CActive*)
TBool ActiveShim_Add(TAny* aOld)
    {
    Lock();
    CGameActive* real = FindReal((TUint32)aOld);
    Unlock();
    if (!real)
        {
        return EFalse;
        }
    if (real->Owner() != MyId())
        {
        Log("Add from another thread than the constructor's, ignored", (TUint32)aOld);
        }
    else if (CActiveScheduler::Current() == NULL)
        {
        Log("Add without an installed scheduler, ignored", (TUint32)aOld);
        }
    else if (!real->IsAdded())
        {
        CActiveScheduler::Add(real);
        Log("added to the scheduler of this thread", (TUint32)aOld);
        }
    return ETrue;
    }

// euser 979: CActive::SetActive()
static void ShimActiveSetActive(TAny* aThis)
    {
    Lock();
    CGameActive* real = FindReal((TUint32)aThis);
    Unlock();
    if (!real)
        {
        Log("SetActive on an unknown object, ignored", (TUint32)aThis);
        return;
        }
    if (real->IsActive())
        {
        Log("SetActive while already active, ignored", (TUint32)aThis);
        return;
        }
    real->Arm();
    }

// euser 152 (called from schedshim.cpp): CActive::Cancel()
TBool ActiveShim_Cancel(TAny* aOld)
    {
    Lock();
    CGameActive* real = FindReal((TUint32)aOld);
    Unlock();
    if (!real)
        {
        return EFalse;
        }
    if (real->Owner() != MyId())
        {
        Log("Cancel from another thread, ignored (would wait on the wrong thread)", (TUint32)aOld);
        return ETrue;
        }
    real->Cancel();
    return ETrue;
    }

// euser 1265: CActive::~CActive(), reached through word 2 of the game's vtable (flags & 1 = free the
// memory) or as the base destructor of a derived destructor.
static void ShimActiveDtor(TAny* aThis, TInt aFlags)
    {
    CGameActive* real = NULL;
    Lock();
    for (TInt i = 0; i < KMaxActive; i++)
        {
        if (gAct[i].iOld == (TUint32)aThis && aThis)
            {
            real = gAct[i].iReal;
            gAct[i].iOld = 0;
            gAct[i].iReal = NULL;
            break;
            }
        }
    Unlock();
    if (real)
        {
        if (real->Owner() == MyId())
            {
            real->Cancel();                     // no-op when idle
            delete real;                        // leaves the scheduler's list
            }
        else
            {
            Log("deleted from another thread, the real object is leaked", (TUint32)aThis);
            }
        }
    Log("~CActive", (TUint32)aThis, aFlags);
    if (aFlags & 1)
        {
        User::Free(aThis);
        }
    }

// euser 962: TInt CActive::RunError(TInt aError) -- the base implementation (the vtable slot of a
// game class that does not override it points at this import)
static TInt ShimActiveRunError(TAny* /*aThis*/, TInt aError)
    {
    return aError;
    }

// euser 927: void RThread::RequestComplete(TRequestStatus*& aStatus, TInt aReason) const
//   r0 = the RThread (the thread that OWNS the request), r1 = address of the status pointer, r2 = reason
static void ShimRequestComplete(RThread* aThis, TRequestStatus** aStatusRef, TInt aReason)
    {
	CrashCrumb(9, (TUint32)*aStatusRef, (TUint32)aReason, (TUint32)__builtin_return_address(0));
    TRequestStatus* st = *aStatusRef;
    Lock();
    CGameActive* real = FindByStatus((TUint32)st);
    Unlock();
    if (!real)
        {
        aThis->RequestComplete(*aStatusRef, aReason);       // a plain status word, same on both sides
        return;
        }
    *(volatile TInt*)st = aReason;                          // the game reads its own status word
    TRequestStatus* rs = real->Status();
    const TBool pending = (rs->Int() == KRequestPending);
    if (gLog && gCompleteLogs < 8)
        {
        gCompleteLogs++;
        gLog->Logf(_L8("active: RequestComplete obj=%08x reason=%d real pending=%d"), real->Old(), aReason, (TInt)pending);
        }
    if (pending)                                            // a second completion would be a stray signal
        {
        aThis->RequestComplete(rs, aReason);                // signals the owning thread's scheduler
        }
    *aStatusRef = NULL;
    }

// ---------------------------------------------------------------------------
void ActiveShim_Init(THostLog* aLog)
    {
    gLog = aLog;
    if (!gLockReady && gLock.CreateLocal() == KErrNone)
        {
        gLockReady = ETrue;
        }
    }

static const TShimRegEntry KActiveShimEntries[] =
    {
    { "euser", 1395, (TUint32)&ShimActiveCtor, 0, "activeshim", NULL },
    { "euser", 979,  (TUint32)&ShimActiveSetActive, 0, "activeshim", NULL },
    { "euser", 1265, (TUint32)&ShimActiveDtor, 0, "activeshim", NULL },
    { "euser", 962,  (TUint32)&ShimActiveRunError, 0, "activeshim", NULL },
    { "euser", 927,  (TUint32)&ShimRequestComplete, 0, "activeshim", NULL }
    };

TInt ActiveShimReg_Init()
    {
    return ShimReg_Add(KActiveShimEntries, sizeof(KActiveShimEntries) / sizeof(KActiveShimEntries[0]));
    }
