// Registry of what the old game registers with the framework, captured from its own calls:
//   CCoeAppUi::AddToStackL / RemoveFromStack        -> the controls that receive keys
//   CCoeEnv::AddForegroundObserverL / Remove...     -> the observers told about foreground changes
// This replaces the Captain Dynamo specific offsets (container at appui+0x60, observer at +0x30):
// the game hands us the pointers itself, whatever its object layout is.
#ifndef CTLREG_H
#define CTLREG_H

#include <e32std.h>
#include <w32std.h>
#include "hostlog.h"

// aLo/aHi: address range of the loaded old image (old vtables and code live there).
// aIatLo/aIatHi: address range of its import address table (the thunks of the old image point into it).
void CtlReg_Init(THostLog* aLog, TUint32 aLo, TUint32 aHi, TUint32 aIatLo, TUint32 aIatHi);

// ETrue if aFn is one of the old image's own import thunks (ARM: ldr rN,[pc,#4]; ldr rN,[rN]; bx rN with the
// IAT slot address as the literal, or the 12-byte ldr rN,[pc]; ldr pc,[rN] shape). A vtable word that points at
// such a thunk is an INHERITED framework function, i.e. the game does not override it. Thumb is not handled
// (plan 5.6): returns EFalse for an address with bit 0 set.
TBool CtlReg_IsImportThunk(TUint32 aFn);

// Calls HandleGainingForeground (aGain) or HandleLosingForeground on every registered observer.
// KErrNotReady (silently) when nothing is registered.
TInt CtlReg_CallForeground(THostLog& aLog, TBool aGain);

// The old AppUi's own overrides. Vtable words are counted from the header (word 0-1 = offset, rtti; word 2 =
// destructor): word 8 = CCoeAppUi::HandleKeyEventL(const TKeyEvent&, TEventCode), word 9 =
// CAknAppUi::HandleForegroundEventL(TBool), word 15 = ConstructL, word 19 = HandleCommandL.
const TInt KAppUiWordKey = 8;
const TInt KAppUiWordForeground = 9;

// ETrue if the game's AppUi overrides that word (it does not point at an import thunk).
TBool CtlReg_AppUiOverrides(TAny* aAppUi, TInt aWord);

// Key event for the old AppUi, the way the framework does it: if the game overrides HandleKeyEventL it gets
// the event (and the control stack is NOT consulted, as in the real framework); otherwise the default of
// the framework runs, which is the control stack (CtlReg_OfferKey). aType is the TEventCode.
TInt CtlReg_AppUiKey(THostLog& aLog, TAny* aAppUi, const TKeyEvent& aKey, TInt aType, TInt& aConsumed);

// Calls the AppUi's HandleForegroundEventL(aGain) when the game overrides it. aCalled tells whether it did.
TInt CtlReg_AppUiForeground(THostLog& aLog, TAny* aAppUi, TBool aGain, TBool& aCalled);

TInt CtlReg_StackCount();           // controls the game put on the stack
TInt CtlReg_ObserverCount();        // foreground observers the game registered

// One line in the log: which route keys and foreground events take for this game.
void CtlReg_LogRoute(THostLog& aLog, TAny* aAppUi);

// Offers the key event to the registered controls, highest stack priority first (newest first on a
// tie), until one consumes it. aType is the TEventCode. Returns KErrNone and aConsumed, KErrNotReady
// when no control is registered, or the old leave code.
TInt CtlReg_OfferKey(THostLog& aLog, const TKeyEvent& aKey, TInt aType, TInt& aConsumed);

TInt CtlRegShim_Init();

#endif
