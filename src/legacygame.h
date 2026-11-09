// Holds a loaded legacy (pre-Symbian-9) game: image, code chunk, linked IAT and the
// three framework objects its code creates (application, document, AppUi).
#ifndef LEGACYGAME_H
#define LEGACYGAME_H

#include <e32std.h>
#include <f32file.h>
#include <w32std.h>              // TKeyEvent
#include "hostlog.h"
#include "legacyimage.h"
#include "iatfill.h"

class TLegacyGame
    {
public:
    TLegacyGame() : iData(NULL), iImg(NULL), iBase(NULL), iLo(0), iHi(0),
                    iApp(NULL), iDoc(NULL), iAppUi(NULL),
                    iRunning(EFalse), iExited(EFalse), iFocusSent(EFalse) {}

    TInt Load(THostLog& aLog, RFs& aFs, const TDesC& aAppPath,
              THostLog* aLinkReport = NULL);                     // read, parse, chunk, relocate, link
    TInt CreateObjects(THostLog& aLog);            // NewApplication -> CreateDocumentL -> CreateAppUiL
    TInt RunConstructL(THostLog& aLog);            // old AppUi::ConstructL in trace mode
    // Tells the old game about a foreground change: the AppUi's own HandleForegroundEventL (if the game
    // overrides it, vtable word 9) and every foreground observer it registered. The game counts as running
    // (keys are forwarded) from the first gain to the loss, whichever of the two it uses, or neither.
    // Returns KErrNone if the game was told through at least one of them, else KErrNotReady.
    TInt CallForeground(THostLog& aLog, TBool aGain);

    TInt CallWsFocusEvent(THostLog& aLog, TBool aGain);  // old AppUi::HandleWsEventL(EEventFocusGained / EEventFocusLost)

    // Forwards a key event the way the framework does: to the AppUi's HandleKeyEventL (vtable word 8) if the
    // game overrides it, else to the controls it put on the stack (their OfferKeyEventL, vtable word 3).
    // aType is the TEventCode (3 = key down, 2 = key up, 1 = key). Returns KErrNone and the old result
    // (1 = consumed) in aConsumed, or an error (not running, bad vtable, old code left).
    TInt OfferKey(THostLog& aLog, const TKeyEvent& aKey, TInt aType, TInt& aConsumed);

    // Calls the old AppUi::HandleCommandL(aCmd) (AppUi vtable word 19). KErrNone, or the leave reason
    // (a pending host exit, KHostExitLeave, is normal and is performed by the caller).
    TInt HandleCommand(THostLog& aLog, TInt aCmd);

    TBool IsRunning() const { return iRunning; }          // between foreground gain and loss
    // ETrue if the game handles keys in its own AppUi::HandleKeyEventL. Such a game is written for the full
    // sequence of key events a real device delivered (down, key, up), so the host forwards the plain key too.
    TBool KeysViaAppUi() const;
    void Close();
    void MarkExited() { iExited = ETrue; }     // the game called CEikAppUi::Exit() itself
    const CLegacyImage* Image() const { return iImg; }
    TIatLinker& Linker() { return iLinker; }
    const TIatLinker& Linker() const { return iLinker; }
    TAny* OldApp() const { return iApp; }
    const TDesC& GamePath() const { return iGamePath; }   // full path the .app was read from

private:
    TBool CallOld(THostLog& aLog, const TDesC8& aLabel, TAny* aFn, TInt aArg, TInt& aResult);
    const TUint32* VTable(TAny* aObj, TInt aMinWords) const;
    void ReadFileL(RFs& aFs, THostLog& aLog, const TDesC& aPath);

private:
    HBufC8* iData;
    CLegacyImage* iImg;
    RChunk iChunk;
    TIatLinker iLinker;
    TUint8* iBase;
    TUint32 iLo;
    TUint32 iHi;
    TAny* iApp;
    TAny* iDoc;
    TAny* iAppUi;
    TBool iRunning;
    TBool iExited;                  // the game called CEikAppUi::Exit() itself: no more events are sent to it
    TBool iFocusSent;               // the game was told EEventFocusGained and not yet EEventFocusLost
    TFileName iGamePath;
    };

#endif
