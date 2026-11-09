#include "legacygame.h"
#include "oldrt.h"
#include "ctlreg.h"
#include "fsshim.h"
#include "schedshim.h"
#include "activeshim.h"
#include "eikshim.h"
#include "gfxfake.h"
#include "audioshim.h"
#include "threadshim.h"
#include "compatshim.h"
#include "memshim.h"
#include "directbind.h"

static TInt AlignUp(TInt aValue, TInt aTo)
    {
    return (aValue + aTo - 1) & ~(aTo - 1);
    }

// The shim tables of all subsystems, added to the registry in this order before the image is linked.
struct TRegStep
    {
    const char* iName;
    TInt (*iInit)();
    };

static const TRegStep KRegSteps[] =
    {
    { "ctlreg",     CtlRegShim_Init },
    { "fsshim",     FsShimReg_Init },
    { "schedshim",  SchedShimReg_Init },
    { "activeshim", ActiveShimReg_Init },
    { "gfxfake",    GfxShimReg_Init },
    { "audioshim",  AudioShimReg_Init },
    { "threadshim", ThreadShimReg_Init },
    { "compatshim", CompatShim_Init },
    { "memshim",    MemShim_Init },
    { "directbind", DirectBind_Init },
    { "eikshim",    EikShim_Init },
    { "oldrt",      OldRtShim_Init }
    };
const TInt KRegStepCount = sizeof(KRegSteps) / sizeof(KRegSteps[0]);

void TLegacyGame::ReadFileL(RFs& aFs, THostLog& aLog, const TDesC& aPath)
    {
    RFile file;
    User::LeaveIfError(file.Open(aFs, aPath, EFileRead | EFileShareAny));
    CleanupClosePushL(file);
    TInt size = 0;
    User::LeaveIfError(file.Size(size));
    iData = HBufC8::NewL(size);
    TPtr8 ptr = iData->Des();
    User::LeaveIfError(file.Read(ptr));
    CleanupStack::PopAndDestroy();          // file
    iGamePath.Copy(aPath);
    aLog.Logf(_L8("game: read %d bytes"), iData->Length());
    }

TInt TLegacyGame::Load(THostLog& aLog, RFs& aFs, const TDesC& aAppPath, THostLog* aLinkReport)
    {
    TRAPD(err, ReadFileL(aFs, aLog, aAppPath));
    if (err != KErrNone)
        {
        aLog.Logf(_L8("game: cannot read file -> %d"), err);
        return err;
        }
    TRAP(err, iImg = CLegacyImage::NewL(*iData));
    if (err != KErrNone)
        {
        aLog.Logf(_L8("game: cannot parse image -> %d"), err);
        return err;
        }
    err = TOldRt::Init();
    if (err != KErrNone)
        {
        aLog.Logf(_L8("game: old runtime init -> %d"), err);
        return err;
        }

    const TInt importCount = iImg->iOrdinals.Count();
    const TInt codeAligned = AlignUp(iImg->iCodeSize, 16);
    const TInt total = AlignUp(codeAligned + KIatStubSize * importCount, 0x1000);
    err = iChunk.CreateLocalCode(total, total);
    if (err != KErrNone)
        {
        aLog.Logf(_L8("game: code chunk -> %d"), err);
        return err;
        }
    iBase = iChunk.Base();
    iLo = (TUint32)iBase;
    iHi = iLo + iImg->iCodeSize;
    // controls and observers the game registers are captured here; the IAT (right after the text) tells which
    // vtable words are inherited framework functions (import thunks)
    CtlReg_Init(&aLog, iLo, iHi, iLo + iImg->iTextSize, iLo + iImg->iTextSize + 4 * importCount);

    // Every subsystem adds its shim table to the registry; the linker below resolves the imports from it.
    for (TInt i = 0; i < KRegStepCount; i++)
        {
        err = KRegSteps[i].iInit();
        if (err != KErrNone)
            {
            aLog.Logf(_L8("shim registry: %s registration failed -> %d"), KRegSteps[i].iName, err);
            return err;
            }
        }

    TInt applied = 0, bad = 0;
    err = iImg->BuildCodeImage(iBase, (TUint32)iBase, applied, bad);
    aLog.Logf(_L8("game: chunk %08x, relocate -> %d applied=%d outside=%d"), iLo, err, applied, bad);
    if (err != KErrNone || bad != 0)
        {
        return (err != KErrNone) ? err : KErrCorrupt;
        }
    err = iLinker.Link(aLog, *iImg, iBase, iBase + codeAligned, aLinkReport);
    if (err != KErrNone)
        {
        return err;
        }
		
    User::IMB_Range(iBase, iBase + total);
    return KErrNone;
    }

TBool TLegacyGame::CallOld(THostLog& aLog, const TDesC8& aLabel, TAny* aFn, TInt aArg, TInt& aResult)
    {
    aLog.Logf(_L8("calling %S (fn=%08x arg=%08x)"), &aLabel, (TUint32)aFn, (TUint32)aArg);
    aResult = 0;
    const TInt r = TOldRt::Call1(aFn, aArg, aResult);
    if (r == KIatStubHitReason)
        {
        TBuf8<128> what;
        TIatLinker::Describe(*iImg, TIatLinker::LastHit(), what);
        aLog.Logf(_L8("  STOPPED at import #%d: %S"), TIatLinker::LastHit(), &what);
        return EFalse;
        }
    if (r != KErrNone)
        {
        aLog.Logf(_L8("  old code left with %d"), r);
        return EFalse;
        }
    aLog.Logf(_L8("  returned %08x"), (TUint32)aResult);
    return ETrue;
    }

// GCC 2.95 vtable of an old object: vptr[0..1] header, virtual n at vptr[2+n].
const TUint32* TLegacyGame::VTable(TAny* aObj, TInt aMinWords) const
    {
    if (!aObj)
        {
        return NULL;
        }
    const TUint32 vt = *(const TUint32*)aObj;
    if (vt < iLo || vt + aMinWords * 4 > iHi || (vt & 3) != 0)
        {
        return NULL;
        }
    return (const TUint32*)vt;
    }

TInt TLegacyGame::CreateObjects(THostLog& aLog)
    {
    TInt r = 0;
    if (!CallOld(aLog, _L8("NewApplication"), iBase + iImg->ExportEntry(0), 0, r))
        {
        return KErrGeneral;
        }
    iApp = (TAny*)r;
    const TUint32* v = VTable(iApp, 15);
    if (!v)
        {
        aLog.Text(_L8("game: bad application vtable"));
        return KErrCorrupt;
        }
    if (!CallOld(aLog, _L8("CreateDocumentL [vt+14]"), (TAny*)v[14], (TInt)iApp, r))
        {
        return KErrGeneral;
        }
    iDoc = (TAny*)r;
    v = VTable(iDoc, 24);
    if (!v)
        {
        aLog.Text(_L8("game: bad document vtable"));
        return KErrCorrupt;
        }
    if (!CallOld(aLog, _L8("CreateAppUiL [vt+19]"), (TAny*)v[19], (TInt)iDoc, r))
        {
        return KErrGeneral;
        }
    iAppUi = (TAny*)r;
    aLog.Logf(_L8("game: app=%08x doc=%08x appui=%08x"), (TUint32)iApp, (TUint32)iDoc, (TUint32)iAppUi);
    return VTable(iAppUi, 30) ? KErrNone : KErrCorrupt;
    }

TInt TLegacyGame::RunConstructL(THostLog& aLog)
    {
    const TUint32* v = VTable(iAppUi, 30);
    if (!v)
        {
        return KErrCorrupt;
        }
    TInt r = 0;
    TIatLinker::SetTrace(&aLog, iImg, ETrue);
    aLog.Text(_L8("--- old AppUi::ConstructL (trace mode) ---"));
    CallOld(aLog, _L8("AppUi::ConstructL [vt+15]"), (TAny*)v[15], (TInt)iAppUi, r);
    CtlReg_LogRoute(aLog, iAppUi);              // which route keys and foreground events take for this game
    return KErrNone;
    }

void TLegacyGame::Close()
    {
    iLinker.Close();
    iChunk.Close();
    delete iImg;
    iImg = NULL;
    delete iData;
    iData = NULL;
    }

// Foreground: the framework calls the AppUi's HandleForegroundEventL (vtable word 9, only if the game overrides
// it; otherwise word 9 is an import thunk and there is nothing to call) and notifies the foreground observers.
// HandleGainingForeground / HandleLosingForeground are called on every foreground observer the game registered
// with CCoeEnv::AddForegroundObserverL (captured by ctlreg.cpp), with this = the pointer the game passed. Their
// vtable words 2 and 3 come from MCoeForegroundObserver.
// "Running" must not depend on the observers: a game that only overrides HandleForegroundEventL (Another World)
// has none, and its keys were never forwarded.
TInt TLegacyGame::CallForeground(THostLog& aLog, TBool aGain)
    {
    TBool appUiCalled = EFalse;
    CtlReg_AppUiForeground(aLog, iAppUi, aGain, appUiCalled);
    const TInt r = CtlReg_CallForeground(aLog, aGain);
    aLog.Logf(_L8("foreground: gain=%d told AppUi override=%d, observers=%d"), (TInt)aGain, (TInt)appUiCalled,
              CtlReg_ObserverCount());
    iRunning = aGain;                       // keys are forwarded only while the game is in the foreground
    return (r == KErrNone || appUiCalled) ? KErrNone : KErrNotReady;
    }

// Games that override CAknAppUi::HandleWsEventL (AppUi vtable word 3) get the window-server focus
// event a real framework would send: Candy starts its CIdle frame loop on EEventFocusGained (11)
// and stops it on EEventFocusLost (10). A game that does not override it has an import thunk in
// that slot (ldr r12,[pc,#4] / ldr r12,[r12] / bx r12): nothing is delivered then, so existing
// games are not touched.
TInt TLegacyGame::CallWsFocusEvent(THostLog& aLog, TBool aGain)
    {
    if (iExited)
        {
        aLog.Text(_L8("foreground: game already exited, focus event not sent"));
        return KErrNone;
        }
    if (!aGain && !iFocusSent)
        {
        return KErrNone;                    // never told "gained", so no "lost" either
        }
    const TUint32* v = VTable(iAppUi, 4);
    if (!v)
        {
        return KErrNotFound;
        }
    const TUint32 fn = v[3];
    if (fn < iLo || fn + 12 > iHi || (fn & 3) != 0)
        {
        return KErrNotFound;
        }
    const TUint32* code = (const TUint32*)fn;
    if (code[0] == 0xE59FC004 && code[1] == 0xE59CC000 && code[2] == 0xE12FFF1C)
        {
        return KErrNone;                    // inherited from the framework: nothing to deliver
        }
    TUint32 ev[16];                         // old TWsEvent: iType first, the rest zero
    Mem::FillZ(ev, sizeof(ev));
    ev[0] = aGain ? 11 : 10;
    TInt res = 0;
    aLog.Logf(_L8("foreground: sending focus event %d (fn=%08x)"), (TInt)ev[0], fn);
    const TInt reason = TOldRt::CallArgs((TAny*)fn, (TInt)iAppUi, (TInt)ev, 0, 0, res);
    aLog.Logf(_L8("foreground: focus event %d sent to the game's HandleWsEventL (fn=%08x) -> old leave %d"),
              ev[0], fn, reason);
    iFocusSent = aGain;
    return reason;
    }

// Keys: if the game overrides CCoeAppUi::HandleKeyEventL (AppUi vtable word 8) it gets the event; otherwise the
// controls the game put on the stack (CCoeAppUi::AddToStackL, captured by ctlreg.cpp)
// get OfferKeyEventL (CCoeControl vtable word 3), highest priority first. Captain Dynamo handles key DOWN
// (3) and key UP (2), translating the scan code to an action id itself:
//   0x0e left=1, 0x0f right=2, 0x10 up=3, 0x11 down=4, 0x35/0x8d/0xa7 (5 / select)=5,
//   0x37/0x8f (7)=6, 0xa4 left softkey=8, 0xa5 right softkey=9
// and answers 1 (consumed) for both; for EEventKey (1) it answers 0. TKeyEvent has the same layout
// in v2 and v3 (iCode, iScanCode, iModifiers, iRepeats), so the event is passed unchanged.
TInt TLegacyGame::OfferKey(THostLog& aLog, const TKeyEvent& aKey, TInt aType, TInt& aConsumed)
    {
    aConsumed = 0;
    if (!iRunning)
        {
        return KErrNotReady;
        }
    return CtlReg_AppUiKey(aLog, iAppUi, aKey, aType, aConsumed);
    }

TInt TLegacyGame::HandleCommand(THostLog& aLog, TInt aCmd)
    {
    if (!iRunning)
        {
        return KErrNotReady;
        }
    const TUint32* v = VTable(iAppUi, 30);
    if (!v)
        {
        return KErrCorrupt;
        }
    aLog.Logf(_L8("softkey: AppUi::HandleCommandL(%d)"), aCmd);
    TInt res = 0;
    const TInt r = TOldRt::CallArgs((TAny*)v[19], (TInt)iAppUi, aCmd, 0, 0, res);
    if (r != KErrNone)
        {
        aLog.Logf(_L8("softkey: HandleCommandL left with %d"), r);
        }
    return r;
    }

TBool TLegacyGame::KeysViaAppUi() const
    {
    return CtlReg_AppUiOverrides(iAppUi, KAppUiWordKey);
    }
