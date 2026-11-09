// configs.cfg: the user's settings file. It lives next to the game's data files (same directory as the
// game's .app, e.g. E:\symbridge\captaindynamo\configs.cfg).
//
// Keys only valid here:
//   scale_mode=1      1 = original size, 2 = fit keeping the aspect ratio, 3 = the same with a bilinear filter,
//                     4 = full screen, 5 = full screen with a bilinear filter (bilinear = software, smoother,
//                     uses more CPU). The scale key cycles 1 -> 2 -> 3 -> 4 -> 5 -> 1 and saves the mode here.
//   scale_key=1       scan code of the key that cycles the scale mode (1 = Backspace, 0 = none)
//   logging=1         1 = write SymBridge_gui.log 
//                     2 = detailed: also records every memory and allocation call for the crash dump and flushes the log after every line,
//                     0 = write nothing. Read before the game is located, so a profile cannot set it.
//
// Keys valid here AND in a game profile (the profile value wins for that game; see TSharedPrefs):
//   show_fps=0        1 = show the FPS counter (top left, green on black), 0 = hidden. on/off also work
//   max_fps=0         0 = unlimited. For games whose frame loop has no timing of its own (they ran at the speed
//                     of the old phone), e.g. max_fps=20 limits the game to 20 frames per second
//   softkey_left_cmd=0   / softkey_right_cmd=0   : command id the game's AppUi::HandleCommandL gets when the
//   left / right softkey is released (for games whose softkeys are menu commands, 0 = off)

#ifndef HOSTCONFIG_H
#define HOSTCONFIG_H

#include <e32std.h>
#include <f32file.h>
#include "hostlog.h"

// scale_mode values
const TInt KScaleOriginal       = 1;
const TInt KScaleAspect         = 2;
const TInt KScaleAspectBilinear = 3;
const TInt KScaleFull           = 4;
const TInt KScaleFullBilinear   = 5;
const TInt KScaleLast           = KScaleFullBilinear;   // the cycle goes 1 .. KScaleLast, then back to 1

inline TBool HostScale_IsAspect(TInt aMode) { return aMode == KScaleAspect || aMode == KScaleAspectBilinear; }
inline TBool HostScale_IsFull(TInt aMode) { return aMode == KScaleFull || aMode == KScaleFullBilinear; }
inline TBool HostScale_IsBilinear(TInt aMode) { return aMode == KScaleAspectBilinear || aMode == KScaleFullBilinear; }

const TInt KDefaultScaleKey = 196;        // EStdKeyBackspace
const TInt KDefaultShowFps = 0;
const TInt KDefaultMaxFps = 0;          // 0 = the game's idle loop is not limited
const TInt KDefaultSoftCmd = 0;         // 0 = the softkey is not turned into a command
const TInt KDefaultLogging = 1;

// The keys that configs.cfg and a game profile both accept. In configs.cfg every field has a value; in a
// profile a field that was not given is KPrefUnset, and HostConfig_ApplyOverrides leaves that one alone.
const TInt KPrefUnset = -1;

struct TSharedPrefs
    {
    TInt iShowFps;                      // 0 or 1
    TInt iMaxFps;                       // 0 = unlimited, else at most this many game frames per second
    TInt iSoftLeftCmd;                  // command id for the left softkey (scan 164), 0 = none
    TInt iSoftRightCmd;                 // command id for the right softkey (scan 165), 0 = none
    };

enum TSharedKeyResult
    {
    ESharedNotAKey,                     // the name is not one of the shared keys
    ESharedApplied,
    ESharedBadValue
    };

void HostConfig_PrefsDefaults(TSharedPrefs& aPrefs);        // the built-in values
void HostConfig_PrefsUnset(TSharedPrefs& aPrefs);           // every field KPrefUnset (a profile that says nothing)

// If aName is a shared key, parses aValue into the matching field of aPrefs (a bad value leaves it unchanged).
TSharedKeyResult HostConfig_ApplySharedKey(const TDesC8& aName, const TDesC8& aValue, TSharedPrefs& aPrefs);

// Copies every field of aOverride that is not KPrefUnset over aTarget.
void HostConfig_ApplyOverrides(const TSharedPrefs& aOverride, TSharedPrefs& aTarget);

struct THostConfig
    {
    TInt iScaleMode;                    // KScaleOriginal .. KScaleLast
    TInt iScaleKey;
    TInt iLogging;                      // 1 = log to SymBridge_gui.log, 0 = off
    TSharedPrefs iPrefs;                // configs.cfg values; the game profile's overrides are applied on top
    };

// aGamePath = full path of the game's .app; the config is read from / written to its directory.
// Missing or unreadable file: defaults are used and the file is created.
void HostConfig_Load(RFs& aFs, const TDesC& aGamePath, THostConfig& aCfg, THostLog& aLog);

// Rewrites only the scale_mode line (other lines and comments are kept; a leftover scale_filter line and
// its comment are dropped). Returns the error code.
TInt HostConfig_SaveMode(RFs& aFs, const TDesC& aGamePath, TInt aMode, THostLog& aLog);

#endif
