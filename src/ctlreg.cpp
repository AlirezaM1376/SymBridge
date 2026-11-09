#include <e32base.h>
#include "ctlreg.h"
#include "oldrt.h"
#include "shimreg.h"

static THostLog* gLog = NULL;
static TUint32 gLo = 0;
static TUint32 gHi = 0;
static TUint32 gIatLo = 0;
static TUint32 gIatHi = 0;

const TInt KMax = 16;

struct TCtl
    {
    TUint32 iPtr;
    TInt iPrio;
    TInt iSeq;
    };

static TCtl gCtl[KMax];
static TInt gCtlCount = 0;
static TInt gSeq = 0;
static TUint32 gObs[KMax];
static TInt gObsCount = 0;
static TInt gLogs = 0;

void CtlReg_Init(THostLog* aLog, TUint32 aLo, TUint32 aHi, TUint32 aIatLo, TUint32 aIatHi)
    {
    gLog = aLog;
    gLo = aLo;
    gHi = aHi;
    gIatLo = aIatLo;
    gIatHi = aIatHi;
    }

static TBool InIat(TUint32 aAddr)
    {
    return aAddr >= gIatLo && aAddr < gIatHi && ((aAddr - gIatLo) & 3) == 0;
    }

TBool CtlReg_IsImportThunk(TUint32 aFn)
    {
    if ((aFn & 3) != 0 || aFn < gLo || aFn + 16 > gHi)      // also rejects Thumb addresses (bit 0)
        {
        return EFalse;
        }
    const TUint32* p = (const TUint32*)aFn;
    if ((p[0] & 0xFFFF0FFF) == 0xE59F0004)                  // ldr rN,[pc,#4] ; ldr rN,[rN] ; bx rN ; .word IATslot
        {
        return InIat(p[3]);
        }
    if ((p[0] & 0xFFFF0FFF) == 0xE59F0000)                  // ldr rN,[pc,#0] ; ldr pc,[rN] ; .word IATslot
        {
        return InIat(p[2]);
        }
    return EFalse;
    }

// GCC 2.95 vtable of an old object: vptr[0..1] header, virtual n at vptr[2+n]. NULL if the object does
// not look like an old-image object.
static const TUint32* VtOf(TUint32 aObj, TInt aWords)
    {
    if (aObj == 0 || (aObj & 3) != 0)
        {
        return NULL;
        }
    const TUint32 vt = *(const TUint32*)aObj;
    if (vt < gLo || vt + aWords * 4 > gHi || (vt & 3) != 0)
        {
        return NULL;
        }
    return (const TUint32*)vt;
    }

// cone 12: void CCoeAppUi::AddToStackL(CCoeControl*, TInt aPriority, TInt aStackingFlags)
static void ShimAddToStackL(TAny* /*aAppUi*/, TAny* aCtl, TInt aPrio, TInt aFlags)
    {
    const TUint32 p = (TUint32)aCtl;
    for (TInt i = 0; i < gCtlCount; i++)
        {
        if (gCtl[i].iPtr == p)
            {
            gCtl[i].iPrio = aPrio;
            gCtl[i].iSeq = ++gSeq;
            return;
            }
        }
    if (gCtlCount < KMax)
        {
        gCtl[gCtlCount].iPtr = p;
        gCtl[gCtlCount].iPrio = aPrio;
        gCtl[gCtlCount].iSeq = ++gSeq;
        gCtlCount++;
        }
    if (gLog && gLogs < 12)
        {
        gLogs++;
        gLog->Logf(_L8("ctl: AddToStackL(control %08x, priority %d, flags %d) -> %d on the stack"),
                   p, aPrio, aFlags, gCtlCount);
        }
    }

// cone 167: void CCoeAppUi::RemoveFromStack(CCoeControl*)
static void ShimRemoveFromStack(TAny* /*aAppUi*/, TAny* aCtl)
    {
    const TUint32 p = (TUint32)aCtl;
    for (TInt i = 0; i < gCtlCount; i++)
        {
        if (gCtl[i].iPtr == p)
            {
            for (TInt j = i + 1; j < gCtlCount; j++)
                {
                gCtl[j - 1] = gCtl[j];
                }
            gCtlCount--;
            break;
            }
        }
    if (gLog && gLogs < 12)
        {
        gLogs++;
        gLog->Logf(_L8("ctl: RemoveFromStack(control %08x) -> %d on the stack"), p, gCtlCount);
        }
    }

// cone 8: void CCoeEnv::AddForegroundObserverL(MCoeForegroundObserver&): the reference is the pointer
// of the observer base subobject, which is what the observer vtable must be called with.
static void ShimAddForegroundObserverL(TAny* /*aEnv*/, TAny* aObs)
    {
    const TUint32 p = (TUint32)aObs;
    for (TInt i = 0; i < gObsCount; i++)
        {
        if (gObs[i] == p)
            {
            return;
            }
        }
    if (gObsCount < KMax)
        {
        gObs[gObsCount++] = p;
        }
    if (gLog && gLogs < 12)
        {
        gLogs++;
        gLog->Logf(_L8("ctl: AddForegroundObserverL(observer %08x) -> %d registered"), p, gObsCount);
        }
    }

// cone 166: void CCoeEnv::RemoveForegroundObserver(MCoeForegroundObserver&)
static void ShimRemoveForegroundObserver(TAny* /*aEnv*/, TAny* aObs)
    {
    const TUint32 p = (TUint32)aObs;
    for (TInt i = 0; i < gObsCount; i++)
        {
        if (gObs[i] == p)
            {
            for (TInt j = i + 1; j < gObsCount; j++)
                {
                gObs[j - 1] = gObs[j];
                }
            gObsCount--;
            break;
            }
        }
    }

TInt CtlReg_CallForeground(THostLog& aLog, TBool aGain)
    {
    if (gObsCount == 0)
        {
        return KErrNotReady;                        // the caller logs what was reached
        }
    TUint32 list[KMax];                             // a callback may add or remove observers
    const TInt n = gObsCount;
    for (TInt i = 0; i < n; i++)
        {
        list[i] = gObs[i];
        }
    for (TInt i = 0; i < n; i++)
        {
        const TUint32* vt = VtOf(list[i], 4);
        if (!vt)
            {
            aLog.Logf(_L8("foreground: observer %08x has no valid vtable, skipped"), list[i]);
            continue;
            }
        if (CtlReg_IsImportThunk(vt[aGain ? 2 : 3]))
            {
            continue;                               // inherited default: nothing to call
            }
        TInt res = 0;
        const TInt reason = TOldRt::Call1((TAny*)vt[aGain ? 2 : 3], (TInt)list[i], res);
        if (reason != KErrNone)
            {
            aLog.Logf(_L8("foreground: old code left with %d (gain=%d, observer %08x)"), reason, (TInt)aGain, list[i]);
            }
        }
    return KErrNone;
    }

TInt CtlReg_OfferKey(THostLog& aLog, const TKeyEvent& aKey, TInt aType, TInt& aConsumed)
    {
    aConsumed = 0;
    if (gCtlCount == 0)
        {
        return KErrNotReady;
        }
    TCtl order[KMax];                               // highest priority first, newest first on a tie
    const TInt n = gCtlCount;
    for (TInt i = 0; i < n; i++)
        {
        TInt j = i;
        while (j > 0 && (order[j - 1].iPrio < gCtl[i].iPrio ||
                         (order[j - 1].iPrio == gCtl[i].iPrio && order[j - 1].iSeq < gCtl[i].iSeq)))
            {
            order[j] = order[j - 1];
            j--;
            }
        order[j] = gCtl[i];
        }
    for (TInt i = 0; i < n; i++)
        {
        const TUint32 ctl = order[i].iPtr;
        const TUint32* vt = VtOf(ctl, 4);
        if (!vt || CtlReg_IsImportThunk(vt[3]))     // not overridden: the framework default answers "not consumed"
            {
            continue;
            }
        TKeyEvent ev = aKey;                        // the old code reads iScanCode at +4 of this copy
        TInt res = 0;
        const TInt reason = TOldRt::CallArgs((TAny*)vt[3], (TInt)ctl, (TInt)&ev, aType, 0, res);
        if (reason != KErrNone)
            {
            aLog.Logf(_L8("key: old OfferKeyEventL(type %d, scan 0x%x) left with %d"), aType, aKey.iScanCode, reason);
            return reason;
            }
        if (res == 1)                               // EKeyWasConsumed
            {
            aConsumed = 1;
            return KErrNone;
            }
        }
    return KErrNone;
    }

TBool CtlReg_AppUiOverrides(TAny* aAppUi, TInt aWord)
    {
    const TUint32* vt = VtOf((TUint32)aAppUi, aWord + 1);
    return vt && !CtlReg_IsImportThunk(vt[aWord]);
    }

TInt CtlReg_AppUiKey(THostLog& aLog, TAny* aAppUi, const TKeyEvent& aKey, TInt aType, TInt& aConsumed)
    {
    aConsumed = 0;
    if (!CtlReg_AppUiOverrides(aAppUi, KAppUiWordKey))
        {
        return CtlReg_OfferKey(aLog, aKey, aType, aConsumed);       // framework default: the control stack
        }
    const TUint32* vt = VtOf((TUint32)aAppUi, KAppUiWordKey + 1);
    TKeyEvent ev = aKey;                            // the old code reads iScanCode at +4 of this copy
    TInt res = 0;
    const TInt reason = TOldRt::CallArgs((TAny*)vt[KAppUiWordKey], (TInt)aAppUi, (TInt)&ev, aType, 0, res);
    if (reason != KErrNone)
        {
        aLog.Logf(_L8("key: old AppUi::HandleKeyEventL(type %d, scan 0x%x) left with %d"), aType, aKey.iScanCode, reason);
        return reason;
        }
    aConsumed = (res == 1) ? 1 : 0;                 // EKeyWasConsumed
    return KErrNone;
    }

TInt CtlReg_AppUiForeground(THostLog& aLog, TAny* aAppUi, TBool aGain, TBool& aCalled)
    {
    aCalled = EFalse;
    if (!CtlReg_AppUiOverrides(aAppUi, KAppUiWordForeground))
        {
        return KErrNone;
        }
    const TUint32* vt = VtOf((TUint32)aAppUi, KAppUiWordForeground + 1);
    TInt res = 0;
    aCalled = ETrue;
    const TInt reason = TOldRt::CallArgs((TAny*)vt[KAppUiWordForeground], (TInt)aAppUi, aGain ? 1 : 0, 0, 0, res);
    if (reason != KErrNone)
        {
        aLog.Logf(_L8("foreground: old AppUi::HandleForegroundEventL(%d) left with %d"), (TInt)aGain, reason);
        }
    return reason;
    }

TInt CtlReg_StackCount()
    {
    return gCtlCount;
    }

TInt CtlReg_ObserverCount()
    {
    return gObsCount;
    }

void CtlReg_LogRoute(THostLog& aLog, TAny* aAppUi)
    {
    const TBool keyOwn = CtlReg_AppUiOverrides(aAppUi, KAppUiWordKey);
    const TBool fgOwn = CtlReg_AppUiOverrides(aAppUi, KAppUiWordForeground);
    const TPtrC8 keyRoute = keyOwn ? _L8("AppUi::HandleKeyEventL (word 8, own)") : _L8("control stack (framework default)");
    const TPtrC8 fgRoute = fgOwn ? _L8("AppUi::HandleForegroundEventL (word 9, own)") : _L8("no AppUi override");
    aLog.Logf(_L8("input route: key=%S, foreground=%S + %d observer(s), stacked controls=%d"),
              &keyRoute, &fgRoute, gObsCount, gCtlCount);
    const TUint32* vt = VtOf((TUint32)aAppUi, KAppUiWordForeground + 1);
    if (vt)
        {
        aLog.Logf(_L8("input route: AppUi vtable %08x word8=%08x word9=%08x"), (TUint32)vt,
                  vt[KAppUiWordKey], vt[KAppUiWordForeground]);
        }
    }

// cone 82: CCoeAppUi::HandleKeyEventL(const TKeyEvent&, TEventCode), called by a game that overrides the
// method and passes on what it does not handle: the framework default offers the key to the control stack.
static TInt ShimAppUiHandleKeyEventL(TAny* /*aAppUi*/, const TKeyEvent* aKey, TInt aType)
    {
    if (!gLog || !aKey)
        {
        return 0;
        }
    TInt consumed = 0;
    const TInt r = CtlReg_OfferKey(*gLog, *aKey, aType, consumed);
    if (r != KErrNone && r != KErrNotReady)
        {
        TOldRt::Leave(r);                           // old code left inside: pass the leave on
        }
    return consumed;
    }

static const TShimRegEntry KCtlRegShimEntries[] =
    {
    { "cone", 8,   (TUint32)&ShimAddForegroundObserverL, 0, "ctlreg", NULL },
    { "cone", 12,  (TUint32)&ShimAddToStackL, 0, "ctlreg", NULL },
    { "cone", 82,  (TUint32)&ShimAppUiHandleKeyEventL, 0, "ctlreg", NULL },     // CCoeAppUi::HandleKeyEventL
    { "cone", 166, (TUint32)&ShimRemoveForegroundObserver, 0, "ctlreg", NULL },
    { "cone", 167, (TUint32)&ShimRemoveFromStack, 0, "ctlreg", NULL }
    };

TInt CtlRegShim_Init()
    {
    return ShimReg_Add(KCtlRegShimEntries, sizeof(KCtlRegShimEntries) / sizeof(KCtlRegShimEntries[0]));
    }
