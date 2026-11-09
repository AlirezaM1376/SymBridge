// SymBridge host application (S60 3rd Edition): Application / Document / AppUi / Container.
//
// CLhAppUi loads the game found by the locator, links it through the shim registry, runs its construction
// chain and then drives it: foreground events, keys and the exit sequence. CLhContainer shows the fake
// screen of gfxfake.cpp (original size, aspect fit or full screen, nearest or bilinear) and handles the
// scale hot key and the FPS counter. The host shims at the end of this file are the imports that need
// the host's own objects (AppUi, old application, fake CCoeEnv, exit request).
#include <eikstart.h>
#include <aknapp.h>
#include <akndoc.h>
#include <aknappui.h>
#include <avkon.hrh>
#include <eikenv.h>
#include <coecntrl.h>
#include <hal.h>
#include <avkon.rsg>
#include <aknsoundsystem.h>
#include <fbs.h>
#include <eikspane.h>
#include <eikbtgpc.h>
#include "oldrt.h"
#include "crashwatch.h"
#include "gfxfake.h"
#include "fsshim.h"
#include "threadshim.h"
#include "schedshim.h"
#include "audioshim.h"
#include "rtshim.h"
#include "shimreg.h"
#include "hostlog.h"
#include "legacygame.h"
#include "importtest.h"
#include "gamelocator.h"
#include "profile.h"
#include "ctlreg.h"
#include "hostconfig.h"
#include "swscale.h"
#include "symbridge.hrh"

_LIT8(KBuildId, "build: 1.1.0 " __DATE__ " " __TIME__);
static void HostDoPendingExitL();
static TInt HostShim_Init();                 // registers the host-object shims (defined below)
static TBool gExitRequested = EFalse;           // the game called CEikAppUi::Exit()
static TInt gKeysPlain = 0;                     // profile keys_plain: the control-stack route also gets the plain EEventKey
const TInt KHostExitLeave = -9001;              // old leave code used to unwind the old frames
_LIT(KLogName, "SymBridge_gui.log");

const TUid KUidSymBridge = { 0xE0007312 };

// Host chrome. The game draws its own bottom bar, so both are hidden by default.
// Later these should become runtime settings.
static const TBool KShowStatusPane = EFalse;
static const TBool KShowCba = EFalse;

static TFileName gLogPath;
TBuf8<16> gHostExitKind;

static TBool LinkReportPath(const TDesC& aAppPath, TFileName& aOut)
    {
    const TInt logSlash = gLogPath.LocateReverse('\\');
    const TInt appSlash = aAppPath.LocateReverse('\\');
    if (logSlash < 0 || appSlash < 0)
        {
        return EFalse;
        }
    TPtrC appName(aAppPath.Mid(appSlash + 1));
    const TInt dot = appName.LocateReverse('.');
    if (dot > 0)
        {
        appName.Set(appName.Left(dot));
        }
    aOut.Copy(gLogPath.Left(logSlash + 1));
    aOut.Append(appName);
    aOut.Append(_L("_links.txt"));
    return ETrue;
    }

static void OpenHostLog(RFs& aFs, THostLog& aLog)
    {
    static const TText KDrives[] = { 'E', 'C' };
    TFileName path;
    for (TInt i = 0; i < 2; i++)
        {
        path.Zero();
        path.Append(KDrives[i]);
        path.Append(_L(":\\"));
        path.Append(KLogName);
        if (aLog.Open(aFs, path) == KErrNone)
            {
            gLogPath = path;
            return;
            }
        }
    TFileName priv;
    if (aFs.PrivatePath(priv) == KErrNone)
        {
        aFs.CreatePrivatePath(EDriveC);
        path.Copy(_L("C:"));
        path.Append(priv);
        path.Append(KLogName);
        if (aLog.Open(aFs, path) == KErrNone)
            {
            gLogPath = path;
            }
        }
    }

static TUint32 gLastPresent = 0;
static TInt gSlowKeyLogs = 0;

// ---------------------------------------------------------------------------
class CLhContainer : public CCoeControl
    {
public:
    void ConstructL(const TRect& aRect, THostLog* aLog)
        {
        iLog = aLog;
        CreateWindowL();
        SetRect(aRect);
        iLine1.Copy(_L("SymBridge"));
        iLine2.Copy(_L("GUI host is running"));
        iLine3.Copy(_L("press right softkey to exit"));
        ActivateL();
        }

    ~CLhContainer()
        {
        GfxFake_SetPresentHook(NULL, NULL);
        delete iTimer;
        delete iFrame;
        delete iScaler;
        delete iScaled;
        User::Free(iSrcSnap);
        User::Free(iScaleOut);
        }

    // Shows the game's fake screen, centred on black.
    void StartFramesL(const TSize& aSrcSize, const TSharedPrefs& aProfilePrefs)
        {
        iSrcSize = aSrcSize;                            // the game screen size from the profile
        iSnapBytes = iSrcSize.iWidth * iSrcSize.iHeight * 2;    // RGB565
        iFrame = new (ELeave) CFbsBitmap;
        User::LeaveIfError(iFrame->Create(iSrcSize, EColor64K));
        iLastFrame = -1;
        iShowFrames = ETrue;
        iCfg.iScaleMode = KScaleOriginal;
        iCfg.iScaleKey = KDefaultScaleKey;
        HostConfig_PrefsDefaults(iCfg.iPrefs);
        iSrcSnap = (TUint8*)User::AllocZL(iSnapBytes);          // the one private copy of the game's frame
        iForeground = ETrue;
        iFpsText.Copy(_L("FPS --"));
        iTickUs = 0;
        if (HAL::Get(HALData::ENanoTickPeriod, iTickUs) != KErrNone || iTickUs <= 0)
            {
            iTickUs = 1000;
            }
        if (iGame && iLog)
            {
            // configs.cfg lives next to the game's .app (and its data files)
            HostConfig_Load(CEikonEnv::Static()->FsSession(), iGame->GamePath(), iCfg, *iLog);
            HostConfig_ApplyOverrides(aProfilePrefs, iCfg.iPrefs);      // the game profile wins over configs.cfg
            iLog->Logf(_L8("config: in effect show_fps=%d max_fps=%d softkeys=%d/%d"), iCfg.iPrefs.iShowFps,
                       iCfg.iPrefs.iMaxFps, iCfg.iPrefs.iSoftLeftCmd, iCfg.iPrefs.iSoftRightCmd);
            SchedShim_SetMaxFps(iCfg.iPrefs.iMaxFps);
            if (!iCfg.iLogging)
                {
                iLog->Text(_L8("config: logging=0, the log ends here"));
                iLog->SetEnabled(EFalse);
                }
			gLogLevel = iCfg.iLogging;                       // 0 also stops the crumbs
			iLog->SetFlushEachLine(iCfg.iLogging >= 2);
            }

        iTimer = CPeriodic::NewL(CActive::EPriorityStandard);   // above any game CIdle (they use -20 .. -100)
        iTimer->Start(TTimeIntervalMicroSeconds32(KFrameTimerMicros),
                      TTimeIntervalMicroSeconds32(KFrameTimerMicros),
                      TCallBack(TickCb, this));
        }

    void SetLine2(const TDesC& aText)
        {
        iLine2.Copy(aText.Left(iLine2.MaxLength()));
        DrawNow();
        }

    void SetGame(TLegacyGame* aGame)
        {
        iGame = aGame;
        GfxFake_SetPresentHook(PresentNow, this);
        }

    // While the host is in the background nothing is copied, scaled or drawn (the game keeps running
    // by itself). Coming back, the next tick redraws at once.
    void SetForeground(TBool aForeground)
        {
        iForeground = aForeground;
        if (aForeground)
            {
            iLastFrame = -1;
            iFpsInit = EFalse;
            }
        }

    // The game must see a key-up for every key still held (app switch, incoming call), or it
    // keeps acting as if the key were down.
    void ReleaseKeys()
        {
        if (!iGame || !iGame->IsRunning() || !iLog)
            {
            return;
            }
        TInt released = 0;
        for (TInt s = 0; s < 256; s++)
            {
            if (iDown[s >> 5] & (1u << (s & 31)))
                {
                iDown[s >> 5] &= ~(1u << (s & 31));
                TKeyEvent ke;
                ke.iCode = 0;
                ke.iScanCode = s;
                ke.iModifiers = 0;
                ke.iRepeats = 0;
                TInt consumed = 0;
                    {
                    TGameCallScope scope;
                    iGame->OfferKey(*iLog, ke, (TInt)EEventKeyUp, consumed);
                    }
                released++;
                }
            }
        if (released)
            {
            iLog->Logf(_L8("key: released %d held key(s) on focus loss"), released);
            }
        }

private:
    void Draw(const TRect& aRect) const
        {
        CWindowGc& gc = SystemGc();
        if (iShowFrames && iFrame)
            {
            DrawGame(gc);
            return;
            }
        gc.SetPenStyle(CGraphicsContext::ENullPen);
        gc.SetBrushStyle(CGraphicsContext::ESolidBrush);
        gc.SetBrushColor(KRgbDarkBlue);
        gc.DrawRect(aRect);
        gc.SetPenStyle(CGraphicsContext::ESolidPen);
        gc.SetPenColor(KRgbWhite);
        gc.UseFont(CEikonEnv::Static()->NormalFont());
        gc.DrawText(iLine1, TPoint(8, 30));
        gc.DrawText(iLine2, TPoint(8, 60));
        gc.DrawText(iLine3, TPoint(8, 90));
        gc.DiscardFont();
        }

    TKeyResponse OfferKeyEventL(const TKeyEvent& aKey, TEventCode aType)
        {		
        // Scale hot key (configs.cfg: scale_key, default Green/Call key). The mode changes on key UP so a held
        // key cannot cycle repeatedly; down, up and the plain key event are all swallowed.
        if (iShowFrames && iCfg.iScaleKey != 0 && aKey.iScanCode == iCfg.iScaleKey)
            {
            if (aType == EEventKeyUp)
                {
                CycleScale();
                }
            return EKeyWasConsumed;
            }

        // Softkeys that are menu commands in the game (configs.cfg: softkey_left_cmd / softkey_right_cmd).
        if (iShowFrames && iGame && iGame->IsRunning() && (aKey.iScanCode == 164 || aKey.iScanCode == 165))
            {
            static TInt softLogs = 0;
            if (iLog && softLogs < 6)
                {
                softLogs++;
                iLog->Logf(_L8("key: softkey scan=%d type=%d code=%d"), aKey.iScanCode, (TInt)aType, aKey.iCode);
                }
            const TInt cmd = (aKey.iScanCode == 164) ? iCfg.iPrefs.iSoftLeftCmd : iCfg.iPrefs.iSoftRightCmd;
            if (cmd != 0)
                {
                if (aType == EEventKeyUp)
                    {
                    iGame->HandleCommand(*iLog, cmd);
                    HostDoPendingExitL();               // leaves if the game asked to exit
                    }
                return EKeyWasConsumed;                 // down, up and key are all swallowed
                }
            }

        // While the game runs it owns the keyboard. It uses key down and key up; the plain
        // EEventKey is swallowed so no default handling reaches the host chrome.
        if (iGame && iGame->IsRunning())
            {
            // A game that handles keys in its own AppUi::HandleKeyEventL was written for the full sequence a
            // real device delivered (down, key, up), so it also gets the plain key; a game that takes keys
            // through the control stack keeps swallowing it.
            if (aType == EEventKeyDown || aType == EEventKeyUp ||
                (aType == EEventKey && (iGame->KeysViaAppUi() || gKeysPlain)) )
                {
                if (aKey.iScanCode >= 0 && aKey.iScanCode < 256)
                    {
                    if (aType == EEventKeyDown)
                        {
                        iDown[aKey.iScanCode >> 5] |= (1u << (aKey.iScanCode & 31));
                        }
                    else if (aType == EEventKeyUp)
                        {
                        iDown[aKey.iScanCode >> 5] &= ~(1u << (aKey.iScanCode & 31));
                        }
                    }
                TInt consumed = 0;

                const TInt f0 = GfxFake_FrameCount();
                const TUint32 t0 = User::NTickCount();
                TInt kr;
                    {
                    TGameCallScope scope;
                    kr = iGame->OfferKey(*iLog, aKey, (TInt)aType, consumed);
                    }
                const TUint32 dt = User::NTickCount() - t0;
                if (dt > 100 && gSlowKeyLogs < 20)
                    {
                    gSlowKeyLogs++;
                    iLog->Logf(_L8("key: game handler took %d ms, frames produced inside it: %d"),
                               (TInt)dt, GfxFake_FrameCount() - f0);
                    }

                HostDoPendingExitL();                   // leaves if the game asked to exit
                if (kr == KErrNone)
                    {
                    return consumed ? EKeyWasConsumed : EKeyWasNotConsumed;
                    }
                // forwarding failed: fall back to the host handling below
                }
            else
                {
                return EKeyWasConsumed;
                }
            }
        // Fallback only (game not running or forwarding failed): the right softkey (scan 165,
        // released) exits the app.
        if (!KShowCba && aType == EEventKeyUp && aKey.iScanCode == 165)
            {
            iAvkonAppUi->HandleCommandL(EAknCmdExit);
            return EKeyWasConsumed;
            }
        if (aType == EEventKey)
            {
            iKeyCount++;
            iLine3.Format(_L("key code=%d scan=%d n=%d"), aKey.iCode, aKey.iScanCode, iKeyCount);
            DrawNow();
            return EKeyWasConsumed;
            }
        return EKeyWasNotConsumed;
        }

private:
    enum { KFrameTimerMicros = 25000 };

    static TInt TickCb(TAny* aSelf)
        {
        ((CLhContainer*)aSelf)->OnTick();
        return 1;
        }

    // Present hook of gfxfake.cpp: a finished game frame while the game is stuck in a long call (its key
    // handler, say) is shown now, because the frame timer cannot run until the call returns.
    static void PresentNow(TAny* aSelf)
        {
        static TBool busy = EFalse;               // never present from inside a present
        if (busy || !GfxFake_InLongGameCall(40))
            {
            return;
            }
        const TUint32 now = User::NTickCount();
        if (now - gLastPresent < 30)
            {
            return;
            }
        gLastPresent = now;
        busy = ETrue;
        CLhContainer* self = (CLhContainer*)aSelf;
        // A leave (e.g. out of memory in the scaler) must not travel through the game's old frames.
        TRAPD(err, self->OnTick());
        CCoeEnv::Static()->WsSession().Flush();
        busy = EFalse;
        static TInt errLogs = 0;
        if (err != KErrNone && errLogs < 3)
            {
            errLogs++;
            self->iLog->Logf(_L8("present: OnTick left with %d inside a game call"), err);
            }
        }

    void OnTick()
        {
        if (!iForeground)
            {
            return;
            }
        const TInt frame = GfxFake_FrameCount();
        const TUint8* src = GfxFake_Pixels();
        const TBool fpsChanged = UpdateFps(frame);
        if (!iFrame || !src || !iSrcSnap || frame == iLastFrame)
            {
            if (fpsChanged && iFrame)
                {
                DrawNow();                                      // no new game frame, but the counter changed
                }
            return;
            }
        iLastFrame = frame;
		CRUMB(12, frame, 0, 0);
        // The live frame is copied exactly once. iSrcSnap is the frame everything else works from, so the
        // nearest and the bilinear picture always show the same frame, and a game write in between
        // can no longer tear one of them.
		Mem::Copy(iSrcSnap, src, iSnapBytes);		
        StoreFrame();
        UpdateScaled();                                         // bilinear only; no-op otherwise
		CRUMB(14, frame, 0, 0);
        DrawNow();
		CRUMB(23, frame, 0, 0);                                 // DrawNow returned
        }

    // iSrcSnap -> iFrame (the bitmap server heap is locked only for this copy).
    void StoreFrame()
        {
        const TInt rowBytes = iSrcSize.iWidth * 2;              // mode 7 = 16 bits per pixel
        iFrame->LockHeap();
        TUint8* dst = (TUint8*)iFrame->DataAddress();
        const TInt dstStride = iFrame->DataStride();
		CRUMB(13, dst, dstStride, 0);
		if (dstStride == rowBytes)
            {
            Mem::Copy(dst, iSrcSnap, iSnapBytes);
            }
        else
            {
            for (TInt y = 0; y < iSrcSize.iHeight; y++)
                {
                Mem::Copy(dst + y * dstStride, iSrcSnap + y * rowBytes, rowBytes);
                }
            }
        iFrame->UnlockHeap();
        }

    // Game frames per second, measured over about a second. Returns ETrue when the text changed.
    TBool UpdateFps(TInt aFrame)
        {
        if (!iCfg.iPrefs.iShowFps)
            {
            return EFalse;
            }
        const TUint32 now = User::NTickCount();
        if (!iFpsInit)
            {
            iFpsInit = ETrue;
            iFpsT0 = now;
            iFpsF0 = aFrame;
            return EFalse;
            }
        const TInt ms = (TInt)(((now - iFpsT0) * (TUint32)iTickUs) / 1000);
        if (ms < 1000)
            {
            return EFalse;
            }
        const TInt fps10 = ((aFrame - iFpsF0) * 10000) / ms;    // tenths of a frame per second
        iFpsT0 = now;
        iFpsF0 = aFrame;
        iFpsText.Format(_L("FPS %d.%d"), fps10 / 10, fps10 % 10);
        return ETrue;
        }

    // Size of the picture for the current scale mode inside the container rectangle.
    //   original size (176x208, centred)   aspect: fit keeping the aspect ratio   full: whole rectangle
    TSize TargetFor(const TRect& aRect) const
        {
        const TSize src(iSrcSize);
        const TInt rw = aRect.Width();
        const TInt rh = aRect.Height();
        if (HostScale_IsFull(iCfg.iScaleMode))
            {
            return TSize(rw, rh);
            }
        if (HostScale_IsAspect(iCfg.iScaleMode) && rw > 0 && rh > 0)
            {
            if (rw * src.iHeight <= rh * src.iWidth)            // width is the limit
                {
                return TSize(rw, (rw * src.iHeight) / src.iWidth);
                }
            return TSize((rh * src.iWidth) / src.iHeight, rh);  // height is the limit (e.g. 203x240)
            }
        return src;
        }

    // 1 -> 2 -> 3 -> 4 -> 5 -> 1 (original, aspect, aspect bilinear, full, full bilinear). The new mode is
    // written back to configs.cfg (only the scale_mode line).
    void CycleScale()
        {
        iCfg.iScaleMode = (iCfg.iScaleMode >= KScaleLast || iCfg.iScaleMode < KScaleOriginal)
                              ? KScaleOriginal : iCfg.iScaleMode + 1;
        UpdateScaled();                                         // new size -> new filtered picture (from iSrcSnap)
        const TSize t = TargetFor(Rect());
        TInt err = KErrNone;
        if (iGame && iLog)
            {
            err = HostConfig_SaveMode(CEikonEnv::Static()->FsSession(), iGame->GamePath(), iCfg.iScaleMode, *iLog);
            iLog->Logf(_L8("scale: mode %d, picture %dx%d, saved -> %d"), iCfg.iScaleMode, t.iWidth, t.iHeight, err);
            }
        DrawNow();                                              // DrawGame clears the borders
        }

    // The container was resized (rotation, status pane): the filtered picture has to match the new size.
    void SizeChanged()
        {
        UpdateScaled();
        }

    // Bilinear scale modes (3 and 5): builds the filtered picture for the current mode and container size.
    // Nothing happens for the other modes or when the target equals the source.
    // On any failure iScaledOk stays false and DrawGame falls back to the nearest-neighbour path.
    void UpdateScaled()
        {
        iScaledOk = EFalse;
        if (!iShowFrames || !iSrcSnap || !HostScale_IsBilinear(iCfg.iScaleMode))
            {
            return;
            }
        const TSize src(iSrcSize);
        const TSize target = TargetFor(Rect());
        if (target == src || target.iWidth < 1 || target.iHeight < 1)
            {
            return;
            }
        TRAPD(err, UpdateScaledL(src, target));
        if (err == KErrNone)
            {
            iScaledOk = ETrue;
            iScaleErrLogged = EFalse;
            }
        else if (!iScaleErrLogged && iLog)
            {
            iScaleErrLogged = ETrue;                            // once, not 40 times a second
            iLog->Logf(_L8("scale: bilinear %dx%d failed (%d), using nearest"), target.iWidth, target.iHeight, err);
            }
        }

    void UpdateScaledL(const TSize& aSrc, const TSize& aTarget)
        {
        const TInt srcRow = aSrc.iWidth * 2;                    // RGB565, no padding in the fake screen
        const TInt dstRow = aTarget.iWidth * 2;
        if (!iScaler || !iScaleOut || !iScaled || iScaler->DstSize() != aTarget)
            {
            delete iScaler;
            iScaler = NULL;
            User::Free(iScaleOut);
            iScaleOut = NULL;
            delete iScaled;
            iScaled = NULL;
            iScaler = CBilinearScaler::NewL(aSrc, aTarget);
            iScaleOut = (TUint8*)User::AllocL(dstRow * aTarget.iHeight);
            CFbsBitmap* bmp = new (ELeave) CFbsBitmap;
            CleanupStack::PushL(bmp);
            User::LeaveIfError(bmp->Create(aTarget, EColor64K));
            CleanupStack::Pop(bmp);
            iScaled = bmp;
            }
        iScaler->Scale(iSrcSnap, srcRow, iScaleOut, dstRow);
        // The heavy work above runs in private memory. The bitmap server heap is locked only for this copy.
        iScaled->LockHeap();
        TUint8* dst = (TUint8*)iScaled->DataAddress();
        const TInt stride = iScaled->DataStride();
        for (TInt y = 0; y < aTarget.iHeight; y++)
            {
            Mem::Copy(dst + y * stride, iScaleOut + y * dstRow, dstRow);
            }
        iScaled->UnlockHeap();
        }

    void DrawGame(CWindowGc& aGc) const
        {
        const TRect r = Rect();
        const TSize target = TargetFor(r);
		CRUMB(15, target.iWidth, target.iHeight, 0);
        const TPoint tl(r.iTl.iX + (r.Width() - target.iWidth) / 2,
                        r.iTl.iY + (r.Height() - target.iHeight) / 2);
        const TRect dst(tl, target);
        aGc.SetPenStyle(CGraphicsContext::ENullPen);
        aGc.SetBrushStyle(CGraphicsContext::ESolidBrush);
        aGc.SetBrushColor(KRgbBlack);
        if (dst.iTl.iY > r.iTl.iY) aGc.DrawRect(TRect(r.iTl.iX, r.iTl.iY, r.iBr.iX, dst.iTl.iY));
        if (dst.iBr.iY < r.iBr.iY) aGc.DrawRect(TRect(r.iTl.iX, dst.iBr.iY, r.iBr.iX, r.iBr.iY));
        if (dst.iTl.iX > r.iTl.iX) aGc.DrawRect(TRect(r.iTl.iX, dst.iTl.iY, dst.iTl.iX, dst.iBr.iY));
        if (dst.iBr.iX < r.iBr.iX) aGc.DrawRect(TRect(dst.iBr.iX, dst.iTl.iY, r.iBr.iX, dst.iBr.iY));
        if (target == iFrame->SizeInPixels())
            {
            aGc.BitBlt(dst.iTl, iFrame);
            }
        else if (iScaledOk && iScaled && iScaled->SizeInPixels() == target)
            {
            aGc.BitBlt(dst.iTl, iScaled);                       // bilinear scale mode
            }
        else
            {
            aGc.DrawBitmap(dst, iFrame);                        // nearest-neighbour modes, or bilinear fallback
            }
        if (iCfg.iPrefs.iShowFps)
            {
            DrawFps(aGc, r.iTl);
            }
        }

    // show_fps=1: small black box with matrix green text in the top left corner of the screen. The box
    // has a fixed width, so it does not jump around as the digits change.
    void DrawFps(CWindowGc& aGc, const TPoint& aTopLeft) const
        {
        _LIT(KWidest, "FPS 88.8");
        const CFont* font = CEikonEnv::Static()->AnnotationFont();
        const TRect box(aTopLeft, TSize(font->TextWidthInPixels(KWidest) + 4, font->HeightInPixels() + 2));
        aGc.SetPenStyle(CGraphicsContext::ENullPen);
        aGc.SetBrushStyle(CGraphicsContext::ESolidBrush);
        aGc.SetBrushColor(KRgbBlack);
        aGc.DrawRect(box);
        aGc.UseFont(font);
        aGc.SetPenStyle(CGraphicsContext::ESolidPen);
        aGc.SetPenColor(TRgb(0x00, 0xFF, 0x41));                // matrix green
        aGc.SetBrushStyle(CGraphicsContext::ENullBrush);
        aGc.DrawText(iFpsText, box, font->AscentInPixels() + 2, CGraphicsContext::ELeft, 3);
        aGc.DiscardFont();
        }

    THostConfig iCfg;               // scale mode, hot key and the preferences: configs.cfg, then the game profile
    TSize iSrcSize;                 // size of the game picture (profile screen size)
    TInt iSnapBytes;                // iSrcSize.iWidth * iSrcSize.iHeight * 2
    TLegacyGame* iGame;
    TUint32 iDown[8];               // scancodes (0..255) of keys currently held, as seen by the game
    CFbsBitmap* iFrame;
    CBilinearScaler* iScaler;       // bilinear modes: tables for the current target size
    CFbsBitmap* iScaled;            // bilinear modes: the filtered picture, target size
    TUint8* iSrcSnap;               // private copy of the game's frame (scaler input)
    TUint8* iScaleOut;              // private scaler output, copied into iScaled under a short lock
    TBool iScaledOk;                // iScaled holds the current frame at the current size
    TBool iForeground;              // host is in the foreground (nothing is drawn otherwise)
    TBuf<16> iFpsText;              // "FPS 25.3"
    TBool iFpsInit;
    TUint32 iFpsT0;                 // tick count at the start of the current measuring window
    TInt iFpsF0;                    // game frame counter at the start of the window
    TInt iTickUs;                   // length of one NTickCount tick in microseconds
    TBool iScaleErrLogged;
    CPeriodic* iTimer;
    TInt iLastFrame;
    TBool iShowFrames;
    THostLog* iLog;
    TBuf<48> iLine1;
    TBuf<48> iLine2;
    TBuf<48> iLine3;
    TInt iKeyCount;
    };

// ---------------------------------------------------------------------------
class CLhAppUi;
static void UiExcHandler(TExcType aType);
static CLhAppUi* gHostAppUi = NULL;     // the real v3 AppUi
static TAny* gOldApp = NULL;            // the old application object
static TUint8 gFakeEnv[256];            // stand-in for CCoeEnv with the OLD (v2) layout

class CLhAppUi : public CAknAppUi
    {
public:
    // Public wrappers for protected / const v3 calls used by the shims.
    CAknKeySoundSystem* ProxyKeySounds() { return KeySounds(); }
    void ProxySetKeyBlockMode(TInt aMode) { SetKeyBlockMode((TAknKeyBlockMode)aMode); }
    TRect ProxyClientRect() const { return ClientRect(); }

    void HandleStatusPaneSizeChange()
        {
        CAknAppUi::HandleStatusPaneSizeChange();
        if (iContainer)
            {
            iContainer->SetRect(HostRect());
            }
        }

    // Whole screen when the host chrome is hidden, otherwise what is left over.
    TRect HostRect() const
        {
        return (KShowStatusPane || KShowCba) ? ClientRect() : ApplicationRect();
        }

    void HandleForegroundEventL(TBool aForeground)
        {
        CAknAppUi::HandleForegroundEventL(aForeground);
        if (iContainer)
            {
            iContainer->SetForeground(aForeground);
            }
        if (!aForeground && iContainer)
            {
            iContainer->ReleaseKeys();                  // before the game is told it lost the foreground
            }
        iGame.CallForeground(iLog, aForeground);
        iGame.CallWsFocusEvent(iLog, aForeground);
        }

    void ConstructL()
        {
        BaseConstructL(EAknEnableSkin);
        gHostAppUi = this;

        OpenHostLog(CEikonEnv::Static()->FsSession(), iLog);
        iLog.Logf(_L8("crash watch thread -> %d"), StartCrashWatch(gLogPath));
        User::SetExceptionHandler(UiExcHandler, 0xFFFFFFFF);
        iLog.Text(_L8("SymBridge GUI start"));
        iLog.Text(KBuildId);
        TVersion ver = User::Version();
        iLog.Logf(_L8("OS version %d.%d build %d"), ver.iMajor, ver.iMinor, ver.iBuild);
		iLog.Logf(_L8("heap: size %d max %d"), User::Heap().Size(), User::Heap().MaxLength());
        TInt v;
        if (HAL::Get(HALData::EMemoryRAMFree, v) == KErrNone)
            {
            iRamStart = v;
            iLog.Logf(_L8("free RAM start=%d"), v);
            }
        gHostExitKind.Copy(_L8("normal"));
        if (StatusPane() && !KShowStatusPane)
            {
            StatusPane()->MakeVisible(EFalse);
            }
        if (Cba() && !KShowCba)
            {
            Cba()->MakeVisible(EFalse);
            }
        TRect r = HostRect();
        iLog.Logf(_L8("client rect %d,%d - %d,%d"), r.iTl.iX, r.iTl.iY, r.iBr.iX, r.iBr.iY);
        // Can a GUI process create an executable chunk? (needed for the legacy loader)
        RChunk c;
        TInt e = c.CreateLocalCode(0x1000, 0x1000);
        iLog.Logf(_L8("CreateLocalCode in GUI process -> %d"), e);
        if (e == KErrNone)
            {
            c.Close();
            }

        iContainer = new (ELeave) CLhContainer;
        iContainer->ConstructL(r, &iLog);
        iContainer->SetGame(&iGame);
        AddToStackL(iContainer);
        iLog.Text(_L8("container created and on stack"));

        // Load the legacy game and run its construction chain.
        // Shared layer: find the game under \symbridge (E, F, C) and load its profile by UID3.
        RFs& gfs = CEikonEnv::Static()->FsSession();
        const TInt rtRegErr = RtShim_Init();
        if (rtRegErr != KErrNone)
            {
            iLog.Logf(_L8("shim registry: rtshim registration failed -> %d"), rtRegErr);
            User::Leave(rtRegErr);
            }

        const TInt hostRegErr = HostShim_Init();
        if (hostRegErr != KErrNone)
            {
            iLog.Logf(_L8("shim registry: host shim registration failed -> %d"), hostRegErr);
            User::Leave(hostRegErr);
            }
        TGameInfo* gi = new (ELeave) TGameInfo;
        CleanupStack::PushL(gi);
        TGameProfile prof;
        const TBool found = (GameLocator_Find(gfs, iLog, *gi) == KErrNone);
        if (found)
            {
			Profile_Load(gfs, gi->iUid3, gi->iCodeSize, prof, iLog);
			gKeysPlain = prof.iKeysPlain;
            iLog.Logf(_L8("profile: keys_plain=%d"), prof.iKeysPlain);
            }
        else
            {
            iLog.Text(_L8("locator: no game found"));
            iContainer->SetLine2(_L("No game in \\symbridge"));
            }
        THostLog linkReport;
        if (found && prof.iLinkReport)
            {
            TFileName reportPath;
            if (LinkReportPath(gi->iAppPath, reportPath))
                {
                const TInt reportErr = linkReport.Open(gfs, reportPath);
                if (reportErr == KErrNone)
                    {
                    TBuf8<160> path8;
                    path8.Copy(reportPath);
                    iLog.Logf(_L8("link report: writing %S"), &path8);
                    }
                else
                    {
                    iLog.Logf(_L8("link report: cannot open output (%d)"), reportErr);
                    }
                }
            else
                {
                iLog.Text(_L8("link report: cannot derive output path"));
                }
            }
        const TInt loadErr = found ? iGame.Load(iLog, gfs, gi->iAppPath,
                                                 linkReport.IsOpen() ? &linkReport : NULL) : KErrNotFound;
        linkReport.Close();
        if (found && loadErr == KErrNone &&
            iGame.CreateObjects(iLog) == KErrNone)
            {
            if (gi->iRoot.Length() > 0)
                {
                TFileName selfTestPath(gi->iRoot);
                selfTestPath.Append(_L("selftest"));
                TEntry selfTestEntry;
                if (gfs.Entry(selfTestPath, selfTestEntry) == KErrNone && !selfTestEntry.IsDir())
                    {
                    iLog.Text(_L8("selftest: marker found; checking mapped ordinals on device"));
                    RunImportMapTest(iLog, *iGame.Image());
                    }
                }
            gOldApp = iGame.OldApp();
            // old code reads CCoeEnv members inline: v2 offset 0x3c = iScreen (CWsScreenDevice*)
            GfxFake_Init(&iLog, prof.iScreenW, prof.iScreenH, prof.iDisplayMode);
            FsShim_Init(&iLog, iGame.GamePath());
            ThreadShim_Init(&iLog);
            ThreadShim_SetPolicy(prof.iStackMin, prof.iHighPriorityAs);
            ThreadShim_SetStopState(prof.iStopStateTls, prof.iStopStateField, prof.iStopStateValue);
            SchedShim_Init(&iLog);
            SchedShim_SetAfterCallHook(HostDoPendingExitL);
            AudioShim_Init(&iLog);
            *(TUint32*)(gFakeEnv + 0x3c) = (TUint32)GfxFake_ScreenDevice();
            iGame.RunConstructL(iLog);
            TRAPD(dispErr, iContainer->StartFramesL(TSize(prof.iScreenW, prof.iScreenH), prof.iPrefs));
            const TRect cr = iContainer->Rect();
            iLog.Logf(_L8("disp: start frames -> %d, container rect %d,%d - %d,%d"),
                      dispErr, cr.iTl.iX, cr.iTl.iY, cr.iBr.iX, cr.iBr.iY);
            iGame.CallForeground(iLog, ETrue);
            iGame.CallWsFocusEvent(iLog, ETrue);
			if (prof.iAutoCmd != 0)
                {
                iLog.Logf(_L8("profile: auto_cmd=%d, sending it to the game now"), prof.iAutoCmd);
                iGame.HandleCommand(iLog, prof.iAutoCmd);
                }
            }
        CleanupStack::PopAndDestroy(gi);
        iLog.Text(_L8("start-up done"));
        TTime t0;
        t0.UniversalTime();
        iT0 = t0.Int64();
        }

    THostLog& HostLog() { return iLog; }
    TLegacyGame& Game() { return iGame; }

    ~CLhAppUi()
        {
        StartExitWatchdog(4000);                            // a hang can no longer freeze the app
        iLog.Text(_L8("exit: destructor start"));
        iGame.CallForeground(iLog, EFalse);                 // stops the CIdle and the audio engine
        iGame.CallWsFocusEvent(iLog, EFalse);

        iLog.Text(_L8("exit: foreground lost"));
        if (iGame.Image()) { TIatLinker::DumpHits(iLog, *iGame.Image()); }
        TTime tEnd;
        tEnd.UniversalTime();
        const TInt ms = (TInt)((tEnd.Int64() - iT0) / 1000);
        iLog.Logf(_L8("gfx: total frames %d in %d ms"), GfxFake_FrameCount(), ms);
        // final summary: leak check, exit reason and linker statistics
        TInt ramExit = 0;
        if (HAL::Get(HALData::EMemoryRAMFree, ramExit) == KErrNone)
            {
            iLog.Logf(_L8("free RAM exit=%d delta=%d"), ramExit, ramExit - iRamStart);
            }
        const TInt secs = (TInt)((tEnd.Int64() - iT0) / 1000000);
        const TInt imports = (iGame.Image()) ? iGame.Image()->iOrdinals.Count() : 0;
        const TIatLinker& linker = iGame.Linker();
        const TInt reached = TIatLinker::ReachedCount();
        iLog.Logf(_L8("SUMMARY uid3=%08x imports=%d direct=%d shim=%d trap=%d reached=%d frames=%d exit=%S secs=%d"),
                  (iGame.Image() ? iGame.Image()->iUid3 : 0), imports, linker.iDirect, linker.iShim,
                  linker.iTrap, reached, GfxFake_FrameCount(), &gHostExitKind, secs);

        AudioShim_BeginStop();                              // the game's stop sequence must not open a real stream
        // Graceful stop: let the game finish its own stop sequence (the audio callbacks must still reach it).
        const TBool ended = ThreadShim_StopAndWait(1000);
        iLog.Logf(_L8("exit: game thread %d (1 = ended by itself, 0 = will be killed)"), (TInt)ended);
        AudioShim_Shutdown();                               // from here no audio callback enters old code
        iLog.Text(_L8("exit: before container delete"));

        if (iContainer)
            {
            RemoveFromStack(iContainer);
            delete iContainer;
            }
        iLog.Text(_L8("exit: container deleted"));
        ThreadShim_Shutdown();                              // game threads must be gone before the code chunk is closed
        iLog.Text(_L8("exit: game threads shut down"));
        iGame.Close();
        iLog.Text(_L8("exit: code chunk closed"));
        iLog.Text(_L8("SymBridge GUI exit"));
        iLog.Close();
        }

private:
    void HandleCommandL(TInt aCommand)
        {
        switch (aCommand)
            {
            case EEikCmdExit:
            case EAknSoftkeyExit:
            case EAknCmdExit:
                Exit();
                break;
            case ESbCmdLog:
                {
                iCount++;
                iLog.Logf(_L8("menu: write log line #%d"), iCount);
                TBuf<48> t;
                t.Format(_L("log lines written: %d"), iCount);
                iContainer->SetLine2(t);
                break;
                }
            default:
                break;
            }
        }

private:
    CLhContainer* iContainer;
    THostLog iLog;
    TInt64 iT0;                     // start of the game loop, for the summary line
    TInt iRamStart;
    TLegacyGame iGame;
    TInt iCount;
    };

// ---------------------------------------------------------------------------
class CLhDocument : public CAknDocument
    {
public:
    CLhDocument(CEikApplication& aApp) : CAknDocument(aApp) {}
private:
    CEikAppUi* CreateAppUiL()
        {
        return new (ELeave) CLhAppUi;
        }
    };

class CLhApplication : public CAknApplication
    {
private:
    CApaDocument* CreateDocumentL()
        {
        return new (ELeave) CLhDocument(*this);
        }
    TUid AppDllUid() const
        {
        return KUidSymBridge;
        }
    };

// Called from plain v3 frames (key handler, idle thunk) after old code has returned. Runs the real
// CEikAppUi::Exit(), which leaves; the TRAP only logs the code and
// the same code is left again so CONE handles it exactly as if Exit() had been called directly.
static void HostDoPendingExitL()
    {
    if (!gExitRequested || !gHostAppUi)
        {
        return;
        }
    gExitRequested = EFalse;
    gHostExitKind.Copy(_L8("normal"));
    gHostAppUi->HostLog().Text(_L8("exit: performing the real CEikAppUi::Exit() from host frames"));
    TInt err = KErrNone;
    TRAP(err, gHostAppUi->Exit());
    gHostAppUi->HostLog().Logf(_L8("exit: CEikAppUi::Exit() left with %d"), err);
    if (err != KErrNone)
        {
        User::Leave(err);
        }
    }

// ---------------------------------------------------------------------------
// Host shims: old import -> real v3 host object. Called with the old calling
// convention (same as EABI for these signatures). Never leave the v3 way.
// ---------------------------------------------------------------------------

static void ShimPopContext(CAknKeySoundSystem* aThis)       // avkon 1213
    { if (aThis) { aThis->PopContext(); } }

static TAny* ShimBaseDtorNoop(TAny* aThis)                   // avkon 2048, cone 232, eikcore 213
    { return aThis; }

// eikcore 67  CEikAppUi::Exit(). The real Exit() ends the app by LEAVING, which is a C++
// exception here; it must never travel through old frames (no unwind info). So: remember the
// request, unwind the old frames with an OLD leave, and let the host call the real Exit()
// from v3 frames (HostDoPendingExitL) once the old call has returned.
static void ShimAppUiExit(TAny*)
    {
    if (!gHostAppUi)
        {
        return;
        }
    gHostAppUi->HostLog().Text(_L8("exit: game called CEikAppUi::Exit(), unwinding the old frames"));
    gHostAppUi->Game().MarkExited();
    gExitRequested = ETrue;
    if (TOldRt::State().iTrapTop)
        {
        TOldRt::Leave(KHostExitLeave);          // never returns
        }
    // no old frame to unwind to: the next host check performs the exit
    }

// euser 397: User::Exit(TInt). On a game thread it ends that thread (as on a phone); on the UI
// thread it must go the same way as CEikAppUi::Exit.
static void ShimUserExit(TInt aReason)
    {
    if (TOldRt::State().iIsGameThread)
        {
        User::Exit(aReason);
        }
    ShimAppUiExit(NULL);
    }

// estlib 104: void exit(int). Snails calls exit(0) after a fatal error. The real exit() would end the whole
// host process, so it becomes the same request as CEikAppUi::Exit(): the old frames are unwound with an old
// leave and the host runs the real Exit() from its own frames. exit() never returns; the leave guarantees
// that as long as old code is running under a trap (it is, for every old call the host makes).
static void ShimCExit(TInt aCode)
    {
    if (gHostAppUi)
        {
        gHostAppUi->HostLog().Logf(_L8("exit: game called exit(%d)"), aCode);
        }
    ShimAppUiExit(NULL);
    }

// euser 809: void User::Panic(const TDesC& aCategory, TInt aReason)   (v3 ordinal 650)
// The panic still ends the process, as it does on a phone, but the category and reason are written to the
// host log first and the SUMMARY line shows exit=panic.
static void ShimUserPanic(const TDesC16* aCategory, TInt aReason)
    {
    gHostExitKind.Copy(_L8("panic"));
    if (gHostAppUi)
        {
        TBuf8<32> cat;
        if (aCategory)
            {
            cat.Copy(aCategory->Left(cat.MaxLength()));
            }
        gHostAppUi->HostLog().Logf(_L8("game panic: category '%S' reason %d"), &cat, aReason);
        }
    if (aCategory)
        {
        User::Panic(*aCategory, aReason);
        }
    User::Panic(_L("OLDGAME"), aReason);
    }

static void UiExcHandler(TExcType aType)
    {
    if (gHostAppUi)
        {
        gHostAppUi->HostLog().Logf(_L8("UI THREAD EXCEPTION type=%d"), (TInt)aType);
        }
    gHostExitKind.Copy(_L8("panic"));
    User::Panic(_L("LH-EXC"), (TInt)aType);
    }

static TAny* ShimNoopArgs(TAny* aThis, TInt, TInt)             // avkon 63 BaseConstructL: host did it
    {
    return aThis;
    }

static CAknKeySoundSystem* ShimKeySounds(TAny*)                // avkon 874
    {
    return gHostAppUi ? gHostAppUi->ProxyKeySounds() : NULL;
    }

// avkon 1272. The old resource id (0x08cc0116) belongs to the v2 avkon.rsg, so it is
// ignored: the v3 default key-sound list is pushed instead (assumption: that is what
// the old id denotes). Cosmetic, so failures are ignored rather than leaving.
static void ShimPushContextL(CAknKeySoundSystem* aThis, TInt)
    {
    if (aThis)
        {
        TRAPD(err, aThis->PushContextL(R_AVKON_DEFAULT_SKEY_LIST));
        (void)err;
        }
    }

static void ShimSetKeyBlockMode(TAny*, TInt aMode)             // avkon 1529
    {
    if (gHostAppUi)
        {
        gHostAppUi->ProxySetKeyBlockMode(aMode);
        }
    }

static TAny* ShimApplication(TAny*)                            // eikcore 17: the OLD application
    {
    return gOldApp;
    }

// eikcore 32: TRect returned through a hidden pointer in r0, this in r1 -- the same
// convention as a free function returning TRect with one pointer argument.
static TRect ShimClientRect(TAny*)
    {
    return gHostAppUi ? gHostAppUi->ProxyClientRect() : TRect();
    }

// eikcore 233: CEikAppUi::ApplicationRect() const, same hidden-pointer convention as ClientRect. The
// application rectangle is the whole area the host gives the game.
static TRect ShimApplicationRect(TAny*)
    {
    return gHostAppUi ? gHostAppUi->ProxyClientRect() : TRect();
    }

static TAny* ShimCoeEnvStatic()                                // cone 223
    {
    return gFakeEnv;
    }

// cone 236: old CCoeControl ctor. Only sets what old code reads inline: iCoeEnv (+8).
static TAny* ShimCoeControlCtor(TAny* aThis)
    {
    *(TUint32*)((TUint8*)aThis + 8) = (TUint32)gFakeEnv;
    return aThis;
    }

// Host shims: entries that need the host objects of this file (AppUi, old application, fake CCoeEnv, exit
// request). Registered explicitly at start-up (HostShim_Init), owner "lhgui". The TTrap entries are in oldrt.cpp.
static const TShimRegEntry KHostShimEntries[] =
    {
    { "avkon",   63,   (TUint32)&ShimNoopArgs,           0, "lhgui", NULL },   // CAknAppUi::BaseConstructL(int)
    { "avkon",   757,  (TUint32)&ShimNoopArgs,           0, "lhgui", NULL },   // CAknAppUi::HandleForegroundEventL(int): called by a game's own override, the host AppUi already ran the base
    { "avkon",   874,  (TUint32)&ShimKeySounds,          0, "lhgui", NULL },   // CAknAppUi::KeySounds()
    { "avkon",   1213, (TUint32)&ShimPopContext,         0, "lhgui", NULL },   // CAknKeySoundSystem::PopContext()
    { "avkon",   1272, (TUint32)&ShimPushContextL,       0, "lhgui", NULL },   // CAknKeySoundSystem::PushContextL(int)
    { "avkon",   1529, (TUint32)&ShimSetKeyBlockMode,    0, "lhgui", NULL },   // CAknAppUi::SetKeyBlockMode(TAknKeyBlockMode)
    { "avkon",   2048, (TUint32)&ShimBaseDtorNoop,       0, "lhgui", NULL },   // CAknAppUi::~CAknAppUi()
    { "avkon",   808,  (TUint32)&ShimNoopArgs,           0, "lhgui", NULL },   // CAknAppUi::HandleWsEventL(...): the game's override calls its base, the host AppUi already did the work
    { "eikcore", 17,   (TUint32)&ShimApplication,        0, "lhgui", NULL },   // CEikAppUi::Application()
    { "eikcore", 32,   (TUint32)&ShimClientRect,         0, "lhgui", NULL },   // CEikAppUi::ClientRect()
    { "eikcore", 233,  (TUint32)&ShimApplicationRect,    0, "lhgui", NULL },   // CEikAppUi::ApplicationRect() const
    { "eikcore", 67,   (TUint32)&ShimAppUiExit,          0, "lhgui", NULL },   // CEikAppUi::Exit()
    { "euser",   397,  (TUint32)&ShimUserExit,           0, "lhgui", NULL },   // User::Exit(int)
	{ "estlib",  104,  (TUint32)&ShimCExit,              0, "lhgui", NULL },   // exit(int)
    { "euser",   809,  (TUint32)&ShimUserPanic,          0, "lhgui", NULL },   // User::Panic(const TDesC&, int)
    { "eikcore", 213,  (TUint32)&ShimBaseDtorNoop,       0, "lhgui", NULL },   // CEikDocument::~CEikDocument()
    { "cone",    223,  (TUint32)&ShimCoeEnvStatic,       0, "lhgui", NULL },   // CCoeEnv::Static()
    { "cone",    232,  (TUint32)&ShimBaseDtorNoop,       0, "lhgui", NULL },   // CCoeControl::~CCoeControl()
    { "cone",    236,  (TUint32)&ShimCoeControlCtor,     0, "lhgui", NULL }    // CCoeControl::CCoeControl()
    };

static TInt HostShim_Init()
    {
    return ShimReg_Add(KHostShimEntries, sizeof(KHostShimEntries) / sizeof(KHostShimEntries[0]));
    }

CApaApplication* NewApplication()
    {
    return new CLhApplication;
    }

GLDEF_C TInt E32Main()
    {
    return EikStart::RunApplication(NewApplication);
    }
