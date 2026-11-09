// Fake "S60v2 graphics hardware" for the legacy game.
// The game renders into a bitmap it creates itself, copies it into the screen memory that
// UserSvr::ScreenInfo reports (direct screen access, +0x20 palette header) and then posts a
// redraw event with UserSvr::AddEvent. Everything here is a stand-in with the OLD layout.
#ifndef GFXFAKE_H
#define GFXFAKE_H

#include <e32std.h>
#include "hostlog.h"

// The screen size and mode come from the game profile (profile.h); 176x208, mode 7 (EColor64K) by default.
const TInt KFakeFbHeader = 0x20;        // palette header in front of the pixels

void GfxFake_Init(THostLog* aLog, TInt aWidth, TInt aHeight, TInt aDisplayMode);
TInt GfxShimReg_Init();
TInt GfxFake_Width();
TInt GfxFake_Height();
TAny* GfxFake_ScreenDevice();                       // old-ABI screen device facade
const TUint8* GfxFake_Pixels();                     // fake screen memory after the header
TInt GfxFake_FrameCount();                          // redraw events seen so far

typedef void (*TFramePresentHook)(TAny* aArg);
void GfxFake_SetPresentHook(TFramePresentHook aHook, TAny* aArg);   // called after every finished game frame

void GfxFake_GameCall(TBool aEnter);                 // UI thread: game code runs / returned (nesting-safe)
TBool GfxFake_InLongGameCall(TUint32 aMinMs);        // same thread only, outermost call running >= aMinMs

// RAII wrapper: the flag is cleared even if a C++ exception (host exit leave) passes through.
class TGameCallScope
    {
public:
    TGameCallScope() { GfxFake_GameCall(ETrue); }
    ~TGameCallScope() { GfxFake_GameCall(EFalse); }
    };

#endif
