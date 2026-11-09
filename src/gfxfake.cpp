// Fake graphics for old (pre-Symbian-9) games: the screen, a bitmap that is only pixel memory, a
// graphics context whose BitBlt really copies, and the direct-screen-access objects around them.
// Everything the game reaches through a vtable of ours answers 0 unless it is implemented here.
#include <e32base.h>
#include <f32file.h>
#include <fbs.h>
#include "gfxfake.h"
#include "shimreg.h"
#include "oldrt.h"
#include "fsshim.h"
#include "crashwatch.h"

static THostLog* gLog = NULL;
static TUint8* gFb = NULL;              // fake screen memory (KFakeFbHeader bytes, then the pixels)
static TInt gFrames = 0;

static TInt gW = 176;
static TInt gH = 208;
static TInt gMode = 7;                  // screen mode the game is told (7 = 64K, 10 = 4K); the fake screen is always RGB565

// ---------------------------------------------------------------------------
// Frame counting and the present hook
// ---------------------------------------------------------------------------
static TFramePresentHook gPresentHook = NULL;
static TAny* gPresentArg = NULL;

void GfxFake_SetPresentHook(TFramePresentHook aHook, TAny* aArg)
    {
    gPresentHook = aHook;
    gPresentArg = aArg;
    }

// A new picture is in the fake screen: count it and let the host show it.
static void FrameDone()
    {
    gFrames++;
	CRUMB(10, gFrames, 0, 0);
    if (gPresentHook)
        {
        gPresentHook(gPresentArg);
		}
	CRUMB(11, gFrames, 0, 0);
    }

// ---------------------------------------------------------------------------
// "Is the game inside a long call?" (the present hook must not draw from a game thread)
// ---------------------------------------------------------------------------
static TInt gGameCallDepth = 0;
static TUint32 gGameCallStart = 0;
static TUint64 gGameCallThread = 0;

void GfxFake_GameCall(TBool aEnter)
    {
    if (aEnter)
        {
        if (gGameCallDepth == 0)
            {
            gGameCallStart = User::NTickCount();
            RThread me;
            gGameCallThread = me.Id().Id();
            }
        gGameCallDepth++;
        }
    else if (gGameCallDepth > 0)
        {
        gGameCallDepth--;
        }
    }

TBool GfxFake_InLongGameCall(TUint32 aMinMs)
    {
    if (gGameCallDepth == 0)
        {
        return EFalse;
        }
    RThread me;
    if (me.Id().Id() != gGameCallThread)     // a game thread that renders must never run host drawing
        {
        return EFalse;
        }
    return (User::NTickCount() - gGameCallStart) >= aMinMs;
    }

// ---------------------------------------------------------------------------
// Fake old-ABI objects and their vtables
// ---------------------------------------------------------------------------
// Old-ABI (GCC 2.95) objects: word 0 of the object points to a vtable that starts with two
// header words; virtual function n is at vtable[2 + n] (slot 0 = destructor). A game calls a
// virtual function with "ldr r12, [vtable, #offset]", so word = offset / 4.
const TInt KFakeVtWords = 128;          // words in every fake vtable
static TUint32 gVtZero[KFakeVtWords];   // every word a harmless "return 0"
static TUint32 gVtDevice[KFakeVtWords]; // + DisplayMode (word 3), SizeInPixels (word 4)
static TUint32 gVtGc[KFakeVtWords];     // fake CBitmapContext: BitBlt (word 48) and BitBlt with a rect (word 49) copy
static TUint32 gVtBitmap[KFakeVtWords]; // fake CFbsBitmap: the destructor gives the pixel memory back
static TUint32 gVtBmpGc[KFakeVtWords]; // fake CFbsBitGc of a bitmap device: BitBlt copies into THAT bitmap
static TUint32 gScreenDev[4];
static TUint32 gFakeGc[4];
static TUint32 gFakeWindow[16];
static TUint32 gFakeRegion[16];

static void FakeDtor(TAny*, TInt) {}

static TInt FakeDisplayMode(TAny*) { return gMode; }

// TSize SizeInPixels() const: hidden result pointer in r0, this in r1 (verified in the binary)
static void FakeSizeInPixels(TAny* aRet, TAny*)
    {
    TInt* r = (TInt*)aRet;
    r[0] = gW;
    r[1] = gH;
    }

// Every word that is not implemented answers 0, but its first calls are logged: a silent 0 can
// be a wrong answer, and the word number is what tells you which virtual function the game wanted
// (see the CBitmapContext vtable in e32/gdi.h). The log is capped so a busy game cannot flood it.
static TInt gWordHits[KFakeVtWords];
static TInt gWordLogs = 0;
static void WordHit(TInt aWord)
    {
    if (gLog && gWordLogs < 16 && gWordHits[aWord] < 2)
        {
        gWordHits[aWord]++;
        gWordLogs++;
        gLog->Logf(_L8("gfx: fake vtable word %d (offset %d) called, answered 0"), aWord, aWord * 4);
        }
    }

// One tiny stub per word; a stub cannot know which word it stands for otherwise.
#define HS(n) static TInt WordStub##n() { WordHit(n); return 0; }
HS(3) HS(4) HS(5) HS(6) HS(7) HS(8) HS(9) HS(10)
HS(11) HS(12) HS(13) HS(14) HS(15) HS(16) HS(17) HS(18)
HS(19) HS(20) HS(21) HS(22) HS(23) HS(24) HS(25) HS(26)
HS(27) HS(28) HS(29) HS(30) HS(31) HS(32) HS(33) HS(34)
HS(35) HS(36) HS(37) HS(38) HS(39) HS(40) HS(41) HS(42)
HS(43) HS(44) HS(45) HS(46) HS(47) HS(48) HS(49) HS(50)
HS(51) HS(52) HS(53) HS(54) HS(55) HS(56) HS(57) HS(58)
HS(59) HS(60) HS(61) HS(62) HS(63) HS(64) HS(65) HS(66)
HS(67) HS(68) HS(69) HS(70) HS(71) HS(72) HS(73) HS(74)
HS(75) HS(76) HS(77) HS(78) HS(79) HS(80) HS(81) HS(82)
HS(83) HS(84) HS(85) HS(86) HS(87) HS(88) HS(89) HS(90)
HS(91) HS(92) HS(93) HS(94) HS(95) HS(96) HS(97) HS(98)
HS(99) HS(100) HS(101) HS(102) HS(103) HS(104) HS(105) HS(106)
HS(107) HS(108) HS(109) HS(110) HS(111) HS(112) HS(113) HS(114)
HS(115) HS(116) HS(117) HS(118) HS(119) HS(120) HS(121) HS(122)
HS(123) HS(124) HS(125) HS(126) HS(127)
#undef HS

#define HF(n) aVt[n] = (TUint32)&WordStub##n;
static void BuildVtable(TUint32* aVt)
    {
    aVt[0] = 0;
    aVt[1] = 0;
    aVt[2] = (TUint32)&FakeDtor;
    HF(3) HF(4) HF(5) HF(6) HF(7) HF(8) HF(9) HF(10)
    HF(11) HF(12) HF(13) HF(14) HF(15) HF(16) HF(17) HF(18)
    HF(19) HF(20) HF(21) HF(22) HF(23) HF(24) HF(25) HF(26)
    HF(27) HF(28) HF(29) HF(30) HF(31) HF(32) HF(33) HF(34)
    HF(35) HF(36) HF(37) HF(38) HF(39) HF(40) HF(41) HF(42)
    HF(43) HF(44) HF(45) HF(46) HF(47) HF(48) HF(49) HF(50)
    HF(51) HF(52) HF(53) HF(54) HF(55) HF(56) HF(57) HF(58)
    HF(59) HF(60) HF(61) HF(62) HF(63) HF(64) HF(65) HF(66)
    HF(67) HF(68) HF(69) HF(70) HF(71) HF(72) HF(73) HF(74)
    HF(75) HF(76) HF(77) HF(78) HF(79) HF(80) HF(81) HF(82)
    HF(83) HF(84) HF(85) HF(86) HF(87) HF(88) HF(89) HF(90)
    HF(91) HF(92) HF(93) HF(94) HF(95) HF(96) HF(97) HF(98)
    HF(99) HF(100) HF(101) HF(102) HF(103) HF(104) HF(105) HF(106)
    HF(107) HF(108) HF(109) HF(110) HF(111) HF(112) HF(113) HF(114)
    HF(115) HF(116) HF(117) HF(118) HF(119) HF(120) HF(121) HF(122)
    HF(123) HF(124) HF(125) HF(126) HF(127)
    }
#undef HF

// Allocates a zeroed old-ABI object of aSize bytes with the given vtable.
static TUint32* NewFake(TInt aSize, const TUint32* aVt)
    {
    TUint32* o = (TUint32*)User::AllocZ(aSize);
    if (o)
        {
        o[0] = (TUint32)aVt;
        }
    return o;
    }

static void ShimGcBitBlt(TAny* aThis, const TInt* aPoint, const TAny* aBitmap);
static void ShimGcBitBlt3(TAny* aThis, const TInt* aPoint, const TAny* aBitmap, const TInt* aRect);
static void BitmapDtor(TAny* aThis, TInt aFlags);
static void ShimBmpGcBitBlt(TAny* aThis, const TInt* aPoint, const TAny* aBitmap);
static void ShimBmpGcBitBlt3(TAny* aThis, const TInt* aPoint, const TAny* aBitmap, const TInt* aRect);
static void ShimGcDrawBitmap(TAny* aThis, const TInt* aRect, const TAny* aBitmap);

void GfxFake_Init(THostLog* aLog, TInt aWidth, TInt aHeight, TInt aDisplayMode)
    {
    gLog = aLog;
    gW = aWidth;
    gH = aHeight;
    gMode = aDisplayMode;

    BuildVtable(gVtZero);
    BuildVtable(gVtDevice);
    gVtDevice[3] = (TUint32)&FakeDisplayMode;
    gVtDevice[4] = (TUint32)&FakeSizeInPixels;
    gScreenDev[0] = (TUint32)gVtDevice;

    BuildVtable(gVtGc);
    gVtGc[48] = (TUint32)&ShimGcBitBlt;     // BitBlt(const TPoint&, const CFbsBitmap*)
    gVtGc[49] = (TUint32)&ShimGcBitBlt3;    // BitBlt(const TPoint&, const CFbsBitmap*, const TRect&)
	gVtGc[56] = (TUint32)&ShimGcDrawBitmap; // DrawBitmap(const TRect&, const CFbsBitmap*): the picture goes to the screen
	gFakeGc[0] = (TUint32)gVtGc;
	
    BuildVtable(gVtBmpGc);
    gVtBmpGc[48] = (TUint32)&ShimBmpGcBitBlt;
    gVtBmpGc[49] = (TUint32)&ShimBmpGcBitBlt3;
	
    gFakeWindow[0] = (TUint32)gVtZero;

    BuildVtable(gVtBitmap);
    gVtBitmap[2] = (TUint32)&BitmapDtor;

    if (!gFb)
        {
        gFb = (TUint8*)User::AllocZ(KFakeFbHeader + gW * gH * 4);   // room for any mode a game asks ScreenInfo for
        }
    if (gLog)
        {
        gLog->Logf(_L8("gfx: fake screen memory at %08x (%dx%d, mode %d)"), (TUint32)gFb, gW, gH, gMode);
        }
    }

TInt GfxFake_Width() { return gW; }
TInt GfxFake_Height() { return gH; }
TAny* GfxFake_ScreenDevice() { return gScreenDev; }
const TUint8* GfxFake_Pixels() { return gFb ? gFb + KFakeFbHeader : NULL; }
TInt GfxFake_FrameCount() { return gFrames; }

// ---------------------------------------------------------------------------
// Shims
// ---------------------------------------------------------------------------
static TAny* ShimNoop() { return NULL; }                       // void / ignored result

static TAny* ShimSystemGc(TAny*) { return gFakeGc; }           // cone 226
static TAny* ShimWindow(TAny*) { return gFakeWindow; }         // cone 231

// ws32 245: TSize RWindowBase::Size() const -- the fake window is as big as the fake screen.
// The TSize comes back through a hidden pointer in r0, this in r1, like ShimClientRect in lhgui.cpp.
static TSize ShimWindowSize(TAny*)
    {
    return TSize(gW, gH);
    }

// euser 970: UserSvr::ScreenInfo(TDes8&) -> SScreenInfoV01 with a VALID screen address
struct TOldScreenInfo
    {
    TInt iWindowHandleValid;
    TAny* iWindowHandle;
    TInt iScreenAddressValid;
    TAny* iScreenAddress;
    TInt iWidth;
    TInt iHeight;
    };

static TInt ShimScreenInfo(TDes8* aInfo)
    {
    TOldScreenInfo si;
    Mem::FillZ(&si, sizeof(si));
    si.iScreenAddressValid = gFb ? 1 : 0;
    si.iScreenAddress = gFb;
    si.iWidth = gW;
    si.iHeight = gH;
    aInfo->Copy((const TUint8*)&si, sizeof(si));
    if (gLog)
        {
        gLog->Logf(_L8("gfx: ScreenInfo -> screen=%08x %dx%d"), (TUint32)gFb, gW, gH);
        }
    return KErrNone;
    }

// euser 1071 / 9: the old TRawEvent is only ever built with Set(type) and handed to AddEvent
static void ShimRawEventSet(TAny* aThis, TInt aType)
    {
    *(TUint32*)aThis = (TUint32)aType;
    }

static TInt ShimAddEvent(const TAny* aEvent)
    {
    const TInt type = (TInt)*(const TUint32*)aEvent;
    if (type == 5)                      // ERedraw: the game has just copied a frame
        {
        FrameDone();
        }
    return KErrNone;
    }

// ws32 348: CDirectScreenAccess::NewL(ws, screen, window, observer) -> fake object
static TAny* ShimDsaNewL(TAny*, TAny*, TAny*, TAny*)
    {
    TUint32* dsa = NewFake(0x40, gVtZero);
    if (dsa)
        {
        dsa[0x18 / 4] = (TUint32)gFakeGc;        // iGc
        dsa[0x1c / 4] = (TUint32)gScreenDev;     // iScreenDevice
        dsa[0x20 / 4] = (TUint32)gFakeRegion;    // iDrawingRegion
        }
    return dsa;
    }

// ---------------------------------------------------------------------------
// CFbsBitmap: the game allocates 0x18 bytes for it and only uses it through the imports below.
// Pixel memory is ours: [+8] pixels, [+c] width, [+10] height, [+14] display mode.
// The pixels are freed when the game deletes the bitmap (vtable word 2, see BitmapDtor) or creates it again.
// ---------------------------------------------------------------------------
static TAny* ShimBitmapCtor(TAny* aThis)                         // fbscli 135
    {
    Mem::FillZ(aThis, 0x18);
    *(TUint32*)aThis = (TUint32)gVtBitmap;
    return aThis;
    }

// Frees the pixel memory and empties the object, so a stale BitBlt from it is ignored (no pixels).
static void FreeBitmapPixels(TUint32* aObj)
    {
    User::Free((TAny*)aObj[2]);
    aObj[2] = 0;
    aObj[3] = 0;
    aObj[4] = 0;
    aObj[5] = 0;
    }

// Destructor of a fake bitmap, called through word 2 of its vtable as dtor(this, flags); flags & 1 = the
// call is a "delete" and the memory of the object itself goes too. It is also the base destructor of a
// game class derived from CFbsBitmap (flags 0): then the game's own delete frees the object.
static void BitmapDtor(TAny* aThis, TInt aFlags)
    {
    FreeBitmapPixels((TUint32*)aThis);
    if (aFlags & 1)
        {
        User::Free(aThis);
        }
    }

static TInt BytesPerPixel(TInt aMode)
    {
    switch (aMode)
        {
        case 7: case 10:      return 2;     // 64K, 4K
        case 8:               return 3;     // 16M
        case 9: case 11:      return 4;     // RGB, 16MU
        default:              return 1;
        }
    }

// Old games render a little past the end of their bitmap (MagicPuz: ~1 row).
// The real bitmap server had room behind the pixels; we must too.
static const TInt KBitmapSlack = 4096;

static TInt gBitmapLogs = 0;
static TInt ShimBitmapCreate(TAny* aThis, const TInt* aSize, TInt aMode)   // fbscli 16
    {
    const TInt w = aSize[0];
    const TInt h = aSize[1];
    if (w <= 0 || h <= 0)
        {
        return KErrArgument;
        }
    const TInt stride = (w * BytesPerPixel(aMode) + 3) & ~3;
    TUint8* px = (TUint8*)User::AllocZ(stride * h + KBitmapSlack);
    if (!px)
        {
        return KErrNoMemory;
        }
    TUint32* o = (TUint32*)aThis;
    FreeBitmapPixels(o);                    // a bitmap that is created again gives its old pixels back
    o[2] = (TUint32)px;
    o[3] = (TUint32)w;
    o[4] = (TUint32)h;
    o[5] = (TUint32)aMode;
    if (gLog && gBitmapLogs < 8)            // the size and mode of a game's bitmaps are what you look for first
        {
        gBitmapLogs++;
        gLog->Logf(_L8("gfx: bitmap %dx%d mode %d stride %d at %08x"), w, h, aMode, stride, (TUint32)px);
        }
    return KErrNone;
    }

static TAny* ShimBitmapDataAddress(const TAny* aThis)            // fbscli 17
    {
    return (TAny*)((const TUint32*)aThis)[2];
    }

// fbscli 21: TDisplayMode CFbsBitmap::DisplayMode() const -- the mode ShimBitmapCreate stored at +0x14
static TInt ShimBitmapDisplayMode(const TAny* aThis)
    {
    return (TInt)((const TUint32*)aThis)[5];
    }

// fbscli 110: TSize CFbsBitmap::SizeInPixels() const -- hidden result pointer in r0, this in r1
static void ShimBitmapSizeInPixels(TAny* aRet, const TAny* aThis)
    {
    const TUint32* o = (const TUint32*)aThis;
    TInt* r = (TInt*)aRet;
    r[0] = (TInt)o[3];
    r[1] = (TInt)o[4];
    }

// ws32 289: CWsBitmap::CWsBitmap(RWsSession&). A CWsBitmap is a CFbsBitmap here (the game allocates 32 bytes,
// the fake needs 0x18); the session is not used. Returns this.
static TAny* ShimWsBitmapCtor(TAny* aThis, TAny* /*aSession*/)
    {
    return ShimBitmapCtor(aThis);
    }

// ---------------------------------------------------------------------------
// eikcore 44: CFbsBitmap* CEikonEnv::CreateBitmapL(const TDesC& aFileName, TInt aId): bitmap number aId of a
// multi-bitmap file (RedHood: 104 of them from \\System\\Apps\\RedHood\\RedHood.mbm, mapped into the game folder).
// The decoding is done by the device: a REAL v3 CFbsBitmap loads the file and GetScanLine converts every row to
// EColor64K (mode 7) into the fake bitmap's pixel memory. A picture that cannot be loaded is still returned, empty
// (BitBlt ignores it): the game keeps running without it, and the first failures are logged with the reason.
// ---------------------------------------------------------------------------
static TInt gMbmLogs = 0;

// The game runs on its own thread, and the bitmap server session is per thread: connect it once here.
// (It stays connected until the thread ends.)
static TBool EnsureFbsSession()
    {
    if (RFbsSession::GetSession())
        {
        return ETrue;
        }
    return RFbsSession::Connect() == KErrNone;
    }
	
static TInt LoadIntoFake(TUint32* aObj, const TDesC16& aFile, TInt aId)
    {
    if (!EnsureFbsSession())
        {
        return KErrCouldNotConnect;
        }
    TFileName mapped;
    const TDesC* nm = &aFile;
    if (FsShim_RemapOldPath(aFile, mapped))
        {
        nm = &mapped;
        }
    CFbsBitmap* bm = new CFbsBitmap;
    if (!bm)
        {
        return KErrNoMemory;
        }
    TInt r = bm->Load(*nm, aId, EFalse);
    if (r == KErrNone)
        {
        const TSize sz = bm->SizeInPixels();
        const TInt stride = (sz.iWidth * 2 + 3) & ~3;
        if (sz.iWidth < 1 || sz.iHeight < 1 || sz.iWidth > 2048 || sz.iHeight > 2048)
            {
            r = KErrCorrupt;
            }
        else
            {
            TUint8* px = (TUint8*)User::AllocZ(stride * sz.iHeight + KBitmapSlack);
            if (!px)
                {
                r = KErrNoMemory;
                }
            else
                {
                for (TInt y = 0; y < sz.iHeight; y++)
                    {
                    TPtr8 row(px + y * stride, 0, stride);
                    bm->GetScanLine(row, TPoint(0, y), sz.iWidth, EColor4K);
                    }
                FreeBitmapPixels(aObj);
                aObj[2] = (TUint32)px;
                aObj[3] = (TUint32)sz.iWidth;
                aObj[4] = (TUint32)sz.iHeight;
                aObj[5] = 10;
                }
            }
        }
    delete bm;
    return r;
    }

static TAny* ShimCreateBitmapL(TAny* /*aEnv*/, const TDesC16* aFile, TInt aId)
    {
    TUint32* b = (TUint32*)User::AllocZ(0x18);
    if (!b)
        {
        TOldRt::Leave(KErrNoMemory);
        }
    ShimBitmapCtor(b);
    const TInt r = aFile ? LoadIntoFake(b, *aFile, aId) : KErrArgument;
    if (gLog && gMbmLogs < 6)                // the first loads: file, number, size, or the reason it failed
        {
        gMbmLogs++;
        TBuf8<100> n;
        if (aFile)
            {
            n.Copy(aFile->Left(n.MaxLength()));
            }
        if (r == KErrNone)
            {
            gLog->Logf(_L8("gfx: mbm '%S' id %d -> %dx%d mode 10"), &n, aId, (TInt)b[3], (TInt)b[4]);
            }
        else
            {
            gLog->Logf(_L8("gfx: mbm '%S' id %d failed (%d) -> empty bitmap"), &n, aId, r);
            }
        }
    return b;
    }
	
// fbscli 76: TInt CFbsBitmap::Load(const TDesC&, TInt aId, TBool aShare) -- .mbm loading is not supported:
// it fails cleanly and the first file names are logged (Cannons' loader function is not reached by any direct call).
// static TInt gLoadLogs = 0;
// static TInt ShimBitmapLoad(TAny*, const TDesC16* aFile, TInt aId, TInt)
    // {
    // if (gLog && gLoadLogs < 4)
        // {
        // gLoadLogs++;
        // TBuf8<100> n;
        // if (aFile)
            // {
            // n.Copy(aFile->Left(n.MaxLength()));
            // }
        // gLog->Logf(_L8("gfx: CFbsBitmap::Load('%S', id %d) not supported -> KErrNotFound"), &n, aId);
        // }
    // return KErrNotFound;
    // }

// fbscli 76: TInt CFbsBitmap::Load(const TDesC& aFileName, TInt aId, TBool aShare). Log(Hic!) loads its gfx
// this way (the other games go through CEikonEnv::CreateBitmapL / ShimCreateBitmapL). Same loader:
// LoadIntoFake remaps the old path and decodes bitmap aId with a real v3 CFbsBitmap into the fake bitmap's
// pixel memory. aShare is irrelevant (the pixels are copied). On failure the fake keeps its previous
// pixels and the error goes back to the game.
static TInt gLoadLogs = 0;
static TInt ShimBitmapLoad(TAny* aThis, const TDesC16* aFile, TInt aId, TInt /*aShare*/)
    {
    const TInt r = aFile ? LoadIntoFake((TUint32*)aThis, *aFile, aId) : KErrArgument;
    if (gLog && gLoadLogs < 4)
        {
        gLoadLogs++;
        TBuf8<100> n;
        if (aFile)
            {
            n.Copy(aFile->Left(n.MaxLength()));
            }
        if (r == KErrNone)
            {
            gLog->Logf(_L8("gfx: CFbsBitmap::Load('%S', id %d) -> %dx%d"), &n, aId,
                       (TInt)((TUint32*)aThis)[3], (TInt)((TUint32*)aThis)[4]);
            }
        else
            {
            gLog->Logf(_L8("gfx: CFbsBitmap::Load('%S', id %d) failed (%d)"), &n, aId, r);
            }
        }
    return r;
    }
	
// cone 160: TRect CCoeControl::Rect() const -- hidden result pointer in r0, this in r1.
// The host owns the real control; every game control is as big as the fake screen.
static void ShimControlRect(TAny* aRet, TAny*)
    {
    TInt* r = (TInt*)aRet;
    r[0] = 0;
    r[1] = 0;
    r[2] = gW;
    r[3] = gH;
    }

// ---------------------------------------------------------------------------
// BitBlt: bitmap -> fake screen (RGB565, rows gW * 2 bytes, after the header)
// ---------------------------------------------------------------------------
// EColor4K is stored as 16-bit words 0x0RGB (4 bits each). One table read per pixel (8 KB table, built once).
static TUint16 gLut4k[4096];
static TBool gLutReady = EFalse;

static void BuildLut4k()
    {
    for (TUint v = 0; v < 4096; v++)
        {
        const TUint r = (v >> 8) & 0xF;
        const TUint g = (v >> 4) & 0xF;
        const TUint b = v & 0xF;
        gLut4k[v] = (TUint16)((((r << 1) | (r >> 3)) << 11) | (((g << 2) | (g >> 2)) << 5) | ((b << 1) | (b >> 3)));
        }
    gLutReady = ETrue;
    }

static void Convert4KTo565(TUint8* aDst, const TUint8* aSrc, TInt aCount)
    {
    if (!gLutReady)
        {
        BuildLut4k();
        }
    const TUint16* s = (const TUint16*)aSrc;     // rows are 4-byte aligned, a column offset keeps 2-byte alignment
    TUint16* d = (TUint16*)aDst;
    TInt i = 0;
    for (; i + 4 <= aCount; i += 4)
        {
        d[i] = gLut4k[s[i] & 0xFFF];
        d[i + 1] = gLut4k[s[i + 1] & 0xFFF];
        d[i + 2] = gLut4k[s[i + 2] & 0xFFF];
        d[i + 3] = gLut4k[s[i + 3] & 0xFFF];
        }
    for (; i < aCount; i++)
        {
        d[i] = gLut4k[s[i] & 0xFFF];
        }
    }

// Copies the part (aSx, aSy, aSw x aSh) of the game's bitmap so that its top left corner lands on the
// point (aDx, aDy) of the fake screen. Both the source part and the destination are clipped, so a
// rectangle or a point outside either is harmless. A bitmap in a mode we cannot show is ignored.
static TInt gIgnoredLogs = 0;
static void BlitToScreen(TInt aDx, TInt aDy, const TAny* aBitmap, TInt aSx, TInt aSy, TInt aSw, TInt aSh)
    {
    const TUint32* b = (const TUint32*)aBitmap;
    const TUint8* px = b ? (const TUint8*)b[2] : NULL;
    const TInt w = b ? (TInt)b[3] : 0;
    const TInt h = b ? (TInt)b[4] : 0;
    const TInt mode = b ? (TInt)b[5] : 0;
	if (!gFb || !px || (mode != 7 && mode != 10))
        {
        if (gLog && gIgnoredLogs < 3)
            {
            gIgnoredLogs++;
            gLog->Logf(_L8("gfx: BitBlt ignored (bitmap %08x, bitmap mode %d, screen mode %d)"),
                       (TUint32)px, mode, gMode);
            }
        return;
        }

    // Clip the source part to the bitmap; the destination moves with it.
    if (aSx < 0)
        {
        aDx -= aSx;
        aSw += aSx;
        aSx = 0;
        }
    if (aSy < 0)
        {
        aDy -= aSy;
        aSh += aSy;
        aSy = 0;
        }
    if (aSx + aSw > w)
        {
        aSw = w - aSx;
        }
    if (aSy + aSh > h)
        {
        aSh = h - aSy;
        }
    // Clip to the screen.
    if (aDx < 0)
        {
        aSx -= aDx;
        aSw += aDx;
        aDx = 0;
        }
    if (aDy < 0)
        {
        aSy -= aDy;
        aSh += aDy;
        aDy = 0;
        }
    if (aDx + aSw > gW)
        {
        aSw = gW - aDx;
        }
    if (aDy + aSh > gH)
        {
        aSh = gH - aDy;
        }

    if (aSw > 0 && aSh > 0)
        {
        const TInt srcStride = (w * 2 + 3) & ~3;
        TUint8* dst = gFb + KFakeFbHeader + (aDy * gW + aDx) * 2;
        const TUint8* src = px + aSy * srcStride + aSx * 2;
        for (TInt y = 0; y < aSh; y++)
            {
            if (mode == 10)
                {
                Convert4KTo565(dst, src, aSw);
                }
            else
                {
                Mem::Copy(dst, src, aSw * 2);
                }
            dst += gW * 2;
            src += srcStride;
            }
        }
    FrameDone();
    }

// CBitmapContext::BitBlt(const TPoint& aPoint, const CFbsBitmap* aBitmap): old ABI, this in r0, the TPoint
// BY REFERENCE in r1, the bitmap in r2 (word 48).
static void ShimGcBitBlt(TAny* /*aThis*/, const TInt* aPoint, const TAny* aBitmap)
    {
    const TUint32* b = (const TUint32*)aBitmap;
    BlitToScreen(aPoint ? aPoint[0] : 0, aPoint ? aPoint[1] : 0, aBitmap, 0, 0,
                 b ? (TInt)b[3] : 0, b ? (TInt)b[4] : 0);
    }

// CBitmapContext::BitBlt(const TPoint& aPoint, const CFbsBitmap* aBitmap, const TRect& aSourceRect):
// the TRect is passed by reference in r3 (iTl.iX, iTl.iY, iBr.iX, iBr.iY) (word 49).
static void ShimGcBitBlt3(TAny* /*aThis*/, const TInt* aPoint, const TAny* aBitmap, const TInt* aRect)
    {
    if (!aRect)
        {
        ShimGcBitBlt(NULL, aPoint, aBitmap);
        return;
        }
    BlitToScreen(aPoint ? aPoint[0] : 0, aPoint ? aPoint[1] : 0, aBitmap,
                 aRect[0], aRect[1], aRect[2] - aRect[0], aRect[3] - aRect[1]);
    }

// ---------------------------------------------------------------------------
// bitgdi
// ---------------------------------------------------------------------------
// bitgdi 86: CFbsBitmapDevice::NewL(CFbsBitmap*) -> fake device (SizeInPixels/DisplayMode work)
static TAny* ShimBitmapDeviceNewL(TAny* aBitmap)
    {
    TUint32* d = NewFake(0x20, gVtDevice);
    if (d)
        {
        d[1] = (TUint32)aBitmap;
        }
    return d;
    }

// bitgdi 23: TInt CFbsDevice::CreateContext(CFbsBitGc*&)
// static TInt ShimCreateContext(TAny*, TAny** aGc)
    // {
    // *aGc = NewFake(0x20, gVtZero);
    // return *aGc ? KErrNone : KErrNoMemory;
    // }

// Copies a part of one fake bitmap into another (a context made by CreateContext draws into its device's bitmap).
// Both must have the same mode (7 or 10); the source part and the destination are clipped.
static TInt gBmpBlitLogs = 0;
static void BlitToBitmap(TUint32* aDst, TInt aDx, TInt aDy, const TUint32* aSrc, TInt aSx, TInt aSy, TInt aSw, TInt aSh)
    {
    TUint8* dp = aDst ? (TUint8*)aDst[2] : NULL;
    const TUint8* sp = aSrc ? (const TUint8*)aSrc[2] : NULL;
    if (!dp || !sp)
        {
        return;
        }
    if (aDst[5] != aSrc[5] || (aSrc[5] != 7 && aSrc[5] != 10))
        {
        if (gLog && gBmpBlitLogs < 3)
            {
            gBmpBlitLogs++;
            gLog->Logf(_L8("gfx: BitBlt into a bitmap ignored (source mode %d, target mode %d)"), (TInt)aSrc[5], (TInt)aDst[5]);
            }
        return;
        }
    const TInt sw0 = (TInt)aSrc[3], sh0 = (TInt)aSrc[4], dw = (TInt)aDst[3], dh = (TInt)aDst[4];
    if (aSx < 0) { aDx -= aSx; aSw += aSx; aSx = 0; }
    if (aSy < 0) { aDy -= aSy; aSh += aSy; aSy = 0; }
    if (aSx + aSw > sw0) { aSw = sw0 - aSx; }
    if (aSy + aSh > sh0) { aSh = sh0 - aSy; }
    if (aDx < 0) { aSx -= aDx; aSw += aDx; aDx = 0; }
    if (aDy < 0) { aSy -= aDy; aSh += aDy; aDy = 0; }
    if (aDx + aSw > dw) { aSw = dw - aDx; }
    if (aDy + aSh > dh) { aSh = dh - aDy; }
    if (aSw <= 0 || aSh <= 0)
        {
        return;
        }
    const TInt ss = (sw0 * 2 + 3) & ~3;
    const TInt ds = (dw * 2 + 3) & ~3;
    for (TInt y = 0; y < aSh; y++)
        {
        Mem::Copy(dp + (aDy + y) * ds + aDx * 2, sp + (aSy + y) * ss + aSx * 2, aSw * 2);
        }
    }

// The context object made by CreateContext: word 0 vtable, word 1 the bitmap of its device.
static void ShimBmpGcBitBlt(TAny* aThis, const TInt* aPoint, const TAny* aBitmap)
    {
    const TUint32* s = (const TUint32*)aBitmap;
    if (s)
        {
        BlitToBitmap((TUint32*)((TUint32*)aThis)[1], aPoint ? aPoint[0] : 0, aPoint ? aPoint[1] : 0,
                     s, 0, 0, (TInt)s[3], (TInt)s[4]);
        }
    }

static void ShimBmpGcBitBlt3(TAny* aThis, const TInt* aPoint, const TAny* aBitmap, const TInt* aRect)
    {
    const TUint32* s = (const TUint32*)aBitmap;
    if (s && aRect)
        {
        BlitToBitmap((TUint32*)((TUint32*)aThis)[1], aPoint ? aPoint[0] : 0, aPoint ? aPoint[1] : 0,
                     s, aRect[0], aRect[1], aRect[2] - aRect[0], aRect[3] - aRect[1]);
        }
    }

// word 56 of the window gc: DrawBitmap(const TRect& aDestRect, const CFbsBitmap*). The host does its own scaling,
// so the picture goes to the screen at the rectangle's top left corner, not stretched.
static TInt gDrawBmpLogs = 0;
static void ShimGcDrawBitmap(TAny* /*aThis*/, const TInt* aRect, const TAny* aBitmap)
    {
    const TUint32* b = (const TUint32*)aBitmap;
    if (!b || !aRect)
        {
        return;
        }
    if (gLog && gDrawBmpLogs < 2)
        {
        gDrawBmpLogs++;
        gLog->Logf(_L8("gfx: DrawBitmap to %d,%d - %d,%d from a %dx%d bitmap (not stretched)"),
                   aRect[0], aRect[1], aRect[2], aRect[3], (TInt)b[3], (TInt)b[4]);
        }
    BlitToScreen(aRect[0], aRect[1], aBitmap, 0, 0, (TInt)b[3], (TInt)b[4]);
    }

// bitgdi 23: TInt CFbsDevice::CreateContext(CFbsBitGc*&). The device of ShimBitmapDeviceNewL knows its bitmap
// (word 1): its context draws into that bitmap. Any other device gets the context that ignores everything.
static TInt ShimCreateContext(TAny* aDevice, TAny** aGc)
    {
    const TUint32* d = (const TUint32*)aDevice;
    const TBool bmp = d && d[0] == (TUint32)gVtDevice && d[1] != 0;
    TUint32* gc = NewFake(0x20, bmp ? gVtBmpGc : gVtZero);
    if (gc && bmp)
        {
        gc[1] = d[1];
        }
    *aGc = gc;
    return gc ? KErrNone : KErrNoMemory;
    }

static const TShimRegEntry KGfxShimEntries[] =
    {
    { "cone", 3,   (TUint32)&ShimNoop, 0, "gfxfake", NULL },
    { "cone", 40,  (TUint32)&ShimNoop, 0, "gfxfake", NULL },
    { "cone", 197, (TUint32)&ShimNoop, 0, "gfxfake", NULL },
    { "cone", 201, (TUint32)&ShimNoop, 0, "gfxfake", NULL },
    { "cone", 315, (TUint32)&ShimNoop, 0, "gfxfake", NULL },
    { "cone", 226, (TUint32)&ShimSystemGc, 0, "gfxfake", NULL },
    { "cone", 231, (TUint32)&ShimWindow, 0, "gfxfake", NULL },
    { "cone", 214, (TUint32)&ShimNoop, 0, "gfxfake", NULL },     // CCoeControl::SetRect(const TRect&): the host owns the real control
    { "ws32", 109, (TUint32)&ShimNoop, 0, "gfxfake", NULL },
    { "ws32", 350, (TUint32)&ShimNoop, 0, "gfxfake", NULL },     // CDirectScreenAccess::StartL()
    { "ws32", 348, (TUint32)&ShimDsaNewL, 0, "gfxfake", NULL },
    { "ws32", 245, (TUint32)&ShimWindowSize, 0, "gfxfake", NULL },
    { "bitgdi", 23, (TUint32)&ShimCreateContext, 0, "gfxfake", NULL },
    { "bitgdi", 86, (TUint32)&ShimBitmapDeviceNewL, 0, "gfxfake", NULL },
    { "bitgdi", 111, (TUint32)&ShimNoop, 0, "gfxfake", NULL },   // CFbsBitGc::SetClippingRegion
    { "bitgdi", 136, (TUint32)&ShimNoop, 0, "gfxfake", NULL },   // CFbsScreenDevice::Update (the BitBlt already presented)
    { "fbscli", 135, (TUint32)&ShimBitmapCtor, 0, "gfxfake", NULL },
    { "fbscli", 16, (TUint32)&ShimBitmapCreate, 0, "gfxfake", NULL },
    { "fbscli", 17, (TUint32)&ShimBitmapDataAddress, 0, "gfxfake", NULL },
    { "euser", 970, (TUint32)&ShimScreenInfo, 0, "gfxfake", NULL },
	{ "cone", 55,  (TUint32)&ShimWindow, 0, "gfxfake", NULL },           // CCoeControl::DrawableWindow() const
    { "cone", 160, (TUint32)&ShimControlRect, 0, "gfxfake", NULL },      // CCoeControl::Rect() const
    { "bitgdi", 112, (TUint32)&ShimNoop, 0, "gfxfake", NULL },           // CFbsBitGc::SetDitherOrigin(const TPoint&)
    { "bitgdi", 120, (TUint32)&ShimNoop, 0, "gfxfake", NULL },           // CFbsBitGc::SetShadowMode(int)
    { "bitgdi", 123, (TUint32)&ShimNoop, 0, "gfxfake", NULL },           // CFbsBitGc::SetUserDisplayMode(TDisplayMode)
    { "fbscli", 21,  (TUint32)&ShimBitmapDisplayMode, 0, "gfxfake", NULL },    // CFbsBitmap::DisplayMode() const
    { "fbscli", 76,  (TUint32)&ShimBitmapLoad, 0, "gfxfake", NULL },           // CFbsBitmap::Load(...)
    { "fbscli", 110, (TUint32)&ShimBitmapSizeInPixels, 0, "gfxfake", NULL },   // CFbsBitmap::SizeInPixels() const
	{ "ws32", 289,   (TUint32)&ShimWsBitmapCtor, 0, "gfxfake", NULL },           // CWsBitmap::CWsBitmap(RWsSession&)
    { "ws32", 58,    (TUint32)&ShimBitmapCreate, 0, "gfxfake", NULL },           // CWsBitmap::Create(const TSize&, TDisplayMode)
    { "eikcore", 44, (TUint32)&ShimCreateBitmapL, 0, "gfxfake", NULL },          // CEikonEnv::CreateBitmapL(const TDesC&, int)
    { "euser", 1071, (TUint32)&ShimRawEventSet, 0, "gfxfake", NULL },
    { "euser", 9, (TUint32)&ShimAddEvent, 0, "gfxfake", NULL }
    };

TInt GfxShimReg_Init()
    {
    return ShimReg_Add(KGfxShimEntries, sizeof(KGfxShimEntries) / sizeof(KGfxShimEntries[0]));
    }
