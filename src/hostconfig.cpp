#include <e32base.h>
#include "hostconfig.h"
#include "cfgtext.h"

_LIT(KCfgName, "configs.cfg");
_LIT8(KKeyMode, "scale_mode");
_LIT8(KKeyKey, "scale_key");
_LIT8(KKeyFps, "show_fps");
_LIT8(KKeyMaxFps, "max_fps");
_LIT8(KKeySoftLeft, "softkey_left_cmd");
_LIT8(KKeySoftRight, "softkey_right_cmd");
_LIT8(KKeyLogging, "logging");


_LIT8(KDefaultText,
"# SymBridge per-game settings. The app rewrites scale_mode when you change it; the rest is yours to edit\r\n"
"# (edit while the app is closed). show_fps, max_fps and the softkey commands can also be set per game\r\n"
"# in the game's profile (c:\\system\\apps\\symbridge\\profiles\\<UID3>.prf); the profile value wins for that game.\r\n"
"#\r\n"
"# scale_mode: 1 = original size, 2 = fit the screen keeping the aspect ratio, 3 = the same with a bilinear\r\n" \
"# filter (smoother, uses more CPU), 4 = stretch to full screen, 5 = full screen with a bilinear filter.\r\n" \
"# The scale key cycles 1 -> 2 -> 3 -> 4 -> 5 -> 1.\r\n"
"scale_mode=1\r\n"
"#\r\n"
"# scale_key: key that cycles the scale mode, as a Symbian scan code (decimal or 0x..).\r\n"
"# Default is 196 = Green Key (Call Key), 0 = no hot key. Avoid keys the game uses: 2, 14-17 (arrows), 42, 53, 55,\r\n"
"# 127, 133, 141, 143, 164, 165, 167.\r\n"
"scale_key=196\r\n"
"#\r\n"
"# show_fps: 1 = show an FPS counter in the top left corner (green on black), 0 = hidden. on / off also work.\r\n"
"show_fps=0\r\n"
"#\r\n"
"# max_fps: 0 = unlimited. A game whose frame loop has no timing of its own runs too fast on a modern phone;\r\n"
"# max_fps=20 (try 15 .. 30) limits it to that many frames per second. Other games: leave 0.\r\n"
"max_fps=0\r\n"
"#\r\n"
"# softkey_left_cmd / softkey_right_cmd: for games whose softkeys are menu commands. The number is the command id\r\n"
"# the game's HandleCommandL gets when the softkey is released. 0 = the softkey is not used.\r\n"
"softkey_left_cmd=0\r\n"
"softkey_right_cmd=0\r\n"
"#\r\n"
"# logging: 0 = off/write nothing, 1 = write SymBridge_gui.log (default), 2 = detailed: also records every memory and\r\n"
"# allocation call for the crash dump and flushes the log after every line (slower; use it to diagnose a crash or a\r\n"
"# freeze). Switch it off to rule logging out as the cause of a slow game.\r\n"
"# Only here: it is read before the game is found, so a profile cannot set it.\r\n"
"logging=1\r\n");

static void ConfigPath(const TDesC& aGamePath, TFileName& aOut)
    {
    TParsePtrC p(aGamePath);
    aOut.Copy(p.DriveAndPath());
    aOut.Append(KCfgName);
    }

static TInt WriteFile(RFs& aFs, const TDesC& aPath, const TDesC8& aData)
    {
    RFile f;
    TInt err = f.Replace(aFs, aPath, EFileWrite);
    if (err == KErrNone)
        {
        err = f.Write(aData);
        f.Close();
        }
    return err;
    }

// Decimal, 0x.. hex
static TBool ParseNumber(const TDesC8& aValue, TInt& aOut)
    {
		return CfgText_ParseNumber(aValue, aOut);
    }

// 0 / 1, or on / off, true / false, yes / no.
static TBool ParseBool(const TDesC8& aValue, TInt& aOut)
    {
    if (aValue.CompareF(_L8("on")) == 0 || aValue.CompareF(_L8("true")) == 0 || aValue.CompareF(_L8("yes")) == 0)
        {
        aOut = 1;
        return ETrue;
        }
    if (aValue.CompareF(_L8("off")) == 0 || aValue.CompareF(_L8("false")) == 0 || aValue.CompareF(_L8("no")) == 0)
        {
        aOut = 0;
        return ETrue;
        }
    return CfgText_ParseNumber(aValue, aOut);
    }

// ---- keys shared with the game profile ----

void HostConfig_PrefsDefaults(TSharedPrefs& aPrefs)
    {
    aPrefs.iShowFps = KDefaultShowFps;
    aPrefs.iMaxFps = KDefaultMaxFps;
    aPrefs.iSoftLeftCmd = KDefaultSoftCmd;
    aPrefs.iSoftRightCmd = KDefaultSoftCmd;
    }

void HostConfig_PrefsUnset(TSharedPrefs& aPrefs)
    {
    aPrefs.iShowFps = KPrefUnset;
    aPrefs.iMaxFps = KPrefUnset;
    aPrefs.iSoftLeftCmd = KPrefUnset;
    aPrefs.iSoftRightCmd = KPrefUnset;
    }

TSharedKeyResult HostConfig_ApplySharedKey(const TDesC8& aName, const TDesC8& aValue, TSharedPrefs& aPrefs)
    {
    TInt n = 0;
    if (aName.CompareF(KKeyFps) == 0)
        {
        if (!ParseBool(aValue, n) || (n != 0 && n != 1))
            {
            return ESharedBadValue;
            }
        aPrefs.iShowFps = n;
        }
    else if (aName.CompareF(KKeyMaxFps) == 0)
        {
        if (!CfgText_ParseNumber(aValue, n) || n < 0 || n > 1000)
            {
            return ESharedBadValue;
            }
        aPrefs.iMaxFps = n;
        }
    else if (aName.CompareF(KKeySoftLeft) == 0)
        {
        if (!CfgText_ParseNumber(aValue, n) || n < 0 || n >= 100000)
            {
            return ESharedBadValue;
            }
        aPrefs.iSoftLeftCmd = n;
        }
    else if (aName.CompareF(KKeySoftRight) == 0)
        {
        if (!CfgText_ParseNumber(aValue, n) || n < 0 || n >= 100000)
            {
            return ESharedBadValue;
            }
        aPrefs.iSoftRightCmd = n;
        }
    else
        {
        return ESharedNotAKey;
        }
    return ESharedApplied;
    }

void HostConfig_ApplyOverrides(const TSharedPrefs& aOverride, TSharedPrefs& aTarget)
    {
    if (aOverride.iShowFps != KPrefUnset)
        {
        aTarget.iShowFps = aOverride.iShowFps;
        }
    if (aOverride.iMaxFps != KPrefUnset)
        {
        aTarget.iMaxFps = aOverride.iMaxFps;
        }
    if (aOverride.iSoftLeftCmd != KPrefUnset)
        {
        aTarget.iSoftLeftCmd = aOverride.iSoftLeftCmd;
        }
    if (aOverride.iSoftRightCmd != KPrefUnset)
        {
        aTarget.iSoftRightCmd = aOverride.iSoftRightCmd;
        }
    }

void HostConfig_Load(RFs& aFs, const TDesC& aGamePath, THostConfig& aCfg, THostLog& aLog)
    {
    aCfg.iScaleMode = KScaleOriginal;
    aCfg.iScaleKey = KDefaultScaleKey;
    aCfg.iLogging = KDefaultLogging;
    HostConfig_PrefsDefaults(aCfg.iPrefs);
    TFileName path;
    ConfigPath(aGamePath, path);
    TBuf8<160> p8;
    p8.Copy(path.Left(p8.MaxLength()));

    HBufC8* text = NULL;
    TRAPD(err, text = CfgText_ReadFileL(aFs, path));
    if (err != KErrNone)
        {
        // Create the file only when it does not exist. Any other failure (in use, too big, no memory)
        // keeps the defaults for this run and leaves the user's file alone.
        TInt w = KErrNone;
        if (err == KErrNotFound || err == KErrPathNotFound)
            {
            w = WriteFile(aFs, path, KDefaultText());
            }
        aLog.Logf(_L8("config: '%S' not readable (%d), defaults in use, file written -> %d"), &p8, err, w);
        return;
        }
    TPtrC8 rest(*text);
    TPtrC8 line;
    while (CfgText_NextLine(rest, line))
        {
        TPtrC8 name;
        TPtrC8 value;
        if (!CfgText_SplitLine(line, name, value))
            {
            continue;
            }
        TInt n = 0;
        TBool ok = ETrue;
        if (name.CompareF(KKeyMode) == 0)
            {
            ok = ParseNumber(value, n) && n >= KScaleOriginal && n <= KScaleLast;
            if (ok) { aCfg.iScaleMode = n; }
            }
        else if (name.CompareF(KKeyKey) == 0)
            {
            ok = ParseNumber(value, n) && n >= 0 && n < 256;
            if (ok) { aCfg.iScaleKey = n; }
            }
        else if (name.CompareF(KKeyLogging) == 0)
            {
			ok = ParseNumber(value, n) && (n == 0 || n == 1 || n == 2);
            if (ok) { aCfg.iLogging = n; }
            }
        else
            {
            const TSharedKeyResult r = HostConfig_ApplySharedKey(name, value, aCfg.iPrefs);
            if (r == ESharedNotAKey)
                {
                TBuf8<48> k;
                k.Copy(name.Left(k.MaxLength()));
                aLog.Logf(_L8("config: unknown key '%S' ignored (profile-only keys do not work here)"), &k);
                continue;
                }
            ok = (r == ESharedApplied);
            }
        if (!ok)
            {
            TBuf8<48> k;
            TBuf8<48> v;
            k.Copy(name.Left(k.MaxLength()));
            v.Copy(value.Left(v.MaxLength()));
            aLog.Logf(_L8("config: bad value '%S' for %S ignored (the default stays)"), &v, &k);
            }
        }
    delete text;

    aLog.Logf(_L8("config: '%S' scale_mode=%d scale_key=%d show_fps=%d max_fps=%d softkeys=%d/%d logging=%d"),
              &p8, aCfg.iScaleMode, aCfg.iScaleKey, aCfg.iPrefs.iShowFps, aCfg.iPrefs.iMaxFps,
              aCfg.iPrefs.iSoftLeftCmd, aCfg.iPrefs.iSoftRightCmd, aCfg.iLogging);
    }

// The old text with its scale_mode line replaced (or appended). Other lines are kept as they are.
static HBufC8* BuildUpdatedL(const TDesC8& aOld, TInt aMode)
    {
    TBuf8<32> modeLine;
    modeLine.Format(_L8("scale_mode=%d\r\n"), aMode);
    // HBufC8* out = HBufC8::NewLC(aOld.Length() * 2 + KModeComment().Length() + 64);   // LF-only input grows by one byte per line
    HBufC8* out = HBufC8::NewLC(aOld.Length() * 2 + 64);   // LF-only input grows by one byte per line
	TPtr8 o = out->Des();
    TBool replaced = EFalse;
    TPtrC8 rest(aOld);
    TPtrC8 line;
    while (CfgText_NextLine(rest, line))
        {
        TPtrC8 name;
        TPtrC8 value;
        const TPtrC8 trimmed = CfgText_Trim(line);
        if (!replaced && CfgText_SplitLine(line, name, value) && name.CompareF(KKeyMode) == 0)
            {
            o.Append(modeLine);
            replaced = ETrue;
            }
        else
            {
            TPtrC8 body(line);
            if (body.Length() > 0 && body[body.Length() - 1] == '\r')
                {
                body.Set(body.Left(body.Length() - 1));
                }
            o.Append(body);
            o.Append(_L8("\r\n"));
            }
        }
    if (!replaced)
        {
        o.Append(modeLine);
        }
    CleanupStack::Pop(out);
    return out;
    }

TInt HostConfig_SaveMode(RFs& aFs, const TDesC& aGamePath, TInt aMode, THostLog& aLog)
    {
    TFileName path;
    ConfigPath(aGamePath, path);
    HBufC8* old = NULL;
    TRAPD(readErr, old = CfgText_ReadFileL(aFs, path));         // a missing file is fine: it is created
    const TDesC8& base = (readErr == KErrNone) ? (const TDesC8&)*old : (const TDesC8&)KDefaultText();
    HBufC8* out = NULL;
    TRAPD(buildErr, out = BuildUpdatedL(base, aMode));
    TInt err = buildErr;
    if (err == KErrNone)
        {
        err = WriteFile(aFs, path, *out);
        }
    delete old;
    delete out;
    if (err != KErrNone)
        {
        aLog.Logf(_L8("config: could not save scale_mode=%d -> %d"), aMode, err);
        }
    return err;
    }
