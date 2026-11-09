#include <e32base.h>
#include "profile.h"
#include "cfgtext.h"

// Fixed location: the profiles are installed with the app (they are part of the .sis).
_LIT(KProfilesDir, "c:\\system\\apps\\symbridge\\profiles\\");
_LIT(KDefaultProfile, "default.prf");
_LIT8(KApplied, "applied");
_LIT8(KNotApplied, "not applied");

static void SetDefaults(TGameProfile& aP)
    {
    aP.iScreenW = 176;
    aP.iScreenH = 208;
    aP.iDisplayMode = 7;
    aP.iStackMin = 0x10000;
    aP.iHighPriorityAs = 0;
    aP.iMatchCodeSize = 0;
    aP.iLinkReport = 0;
	aP.iKeysPlain = 0;
	aP.iAutoCmd = 0;
    aP.iStopStateTls = 0x60;
    aP.iStopStateField = 0x20;
    aP.iStopStateValue = 3;
    HostConfig_PrefsUnset(aP.iPrefs);
    }

// Applies every recognised key of one file onto aP. Returns the file error (KErrNone = read).
static TInt ApplyFile(RFs& aFs, const TDesC& aPath, TGameProfile& aP, THostLog& aLog, const TDesC8& aLabel)
    {
    HBufC8* text = NULL;
    TRAPD(err, text = CfgText_ReadFileL(aFs, aPath));
    if (err != KErrNone)
        {
        return err;
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
        if (name.CompareF(_L8("screen_width")) == 0)
            {
            ok = CfgText_ParseNumber(value, n) && n >= 32 && n <= 640;
            if (ok) { aP.iScreenW = n; }
            }
        else if (name.CompareF(_L8("screen_height")) == 0)
            {
            ok = CfgText_ParseNumber(value, n) && n >= 32 && n <= 640;
            if (ok) { aP.iScreenH = n; }
            }
        else if (name.CompareF(_L8("display_mode")) == 0)
            {
            ok = CfgText_ParseNumber(value, n) && (n == 7 || n == 10);  // EColor64K, or EColor4K (reported only)
			if (ok) { aP.iDisplayMode = n; }
            }
        else if (name.CompareF(_L8("thread_stack_min")) == 0)
            {
            ok = CfgText_ParseNumber(value, n) && n >= 0x4000 && n <= 0x40000;
            if (ok) { aP.iStackMin = n; }
            }
        else if (name.CompareF(_L8("thread_high_priority")) == 0)
            {
            ok = ETrue;
            if (value.CompareF(_L8("less")) == 0) { aP.iHighPriorityAs = -1; }
            else if (value.CompareF(_L8("normal")) == 0) { aP.iHighPriorityAs = 0; }
            else if (value.CompareF(_L8("more")) == 0) { aP.iHighPriorityAs = 1; }
            else { ok = EFalse; }
            }
        else if (name.CompareF(_L8("match_code_size")) == 0)
            {
            ok = CfgText_ParseNumber(value, n) && n >= 0;
            if (ok) { aP.iMatchCodeSize = n; }
            }
        else if (name.CompareF(_L8("link_report")) == 0)
            {
            ok = CfgText_ParseNumber(value, n) && (n == 0 || n == 1);
            if (ok) { aP.iLinkReport = n; }
            }
        else if (name.CompareF(_L8("stop_state_tls")) == 0)
            {
            ok = CfgText_ParseNumber(value, n) && n >= 0 && n <= 0x1000 && (n & 3) == 0;
            if (ok) { aP.iStopStateTls = n; }
            }
        else if (name.CompareF(_L8("stop_state_field")) == 0)
            {
            ok = CfgText_ParseNumber(value, n) && n >= 0 && n <= 0x1000 && (n & 3) == 0;
            if (ok) { aP.iStopStateField = n; }
            }
		else if (name.CompareF(_L8("keys_plain")) == 0)
            {
            ok = CfgText_ParseNumber(value, n) && (n == 0 || n == 1);
            if (ok) { aP.iKeysPlain = n; }
            }
		else if (name.CompareF(_L8("auto_cmd")) == 0)
            {
            ok = CfgText_ParseNumber(value, n) && n >= 0 && n <= 65535;
            if (ok) { aP.iAutoCmd = n; }
            }
        else if (name.CompareF(_L8("stop_state_value")) == 0)
            {
            ok = CfgText_ParseNumber(value, n);
            if (ok) { aP.iStopStateValue = n; }
            }
        else
            {
            const TSharedKeyResult r = HostConfig_ApplySharedKey(name, value, aP.iPrefs);
            if (r == ESharedNotAKey)
                {
                TBuf8<48> k;
                k.Copy(name.Left(k.MaxLength()));
                aLog.Logf(_L8("profile: unknown key '%S' in %S ignored (scale_mode, scale_key and logging belong in configs.cfg)"),
                          &k, &aLabel);
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
            aLog.Logf(_L8("profile: bad value '%S' for %S in %S ignored"), &v, &k, &aLabel);
            }
        }
    delete text;
    return KErrNone;
    }

void Profile_Load(RFs& aFs, TUint32 aUid3, TInt aCodeSize, TGameProfile& aP, THostLog& aLog)
    {
    SetDefaults(aP);

    TFileName path(KProfilesDir);
    path.Append(KDefaultProfile);
    const TInt e1 = ApplyFile(aFs, path, aP, aLog, _L8("default.prf"));
    aP.iMatchCodeSize = 0;                              // only meaningful in a game profile

    TFileName gpath(KProfilesDir);
    gpath.AppendNumFixedWidthUC(aUid3, EHex, 8);
    gpath.Append(_L(".prf"));
    TGameProfile game = aP;
    const TInt e2 = ApplyFile(aFs, gpath, game, aLog, _L8("game profile"));
    TBool used = EFalse;
    if (e2 == KErrNone)
        {
        if (game.iMatchCodeSize != 0 && game.iMatchCodeSize != aCodeSize)
            {
            aLog.Logf(_L8("profile: game profile %08x ignored: match_code_size %x, this .app has %x"),
                      aUid3, game.iMatchCodeSize, aCodeSize);
            }
        else
            {
            aP = game;
            used = ETrue;
            }
        }
    aLog.Logf(_L8("profile: default.prf -> %d, %08X.prf -> %d (%S)"), e1, aUid3, e2,
              used ? &KApplied() : &KNotApplied());
    aLog.Logf(_L8("profile: screen %dx%d mode %d, thread stack min %x, high priority as %d, link_report=%d"),
              aP.iScreenW, aP.iScreenH, aP.iDisplayMode, aP.iStackMin, aP.iHighPriorityAs, aP.iLinkReport);
    aLog.Logf(_L8("profile: exit trick tls=%x field=%x value=%d (tls 0 = off)"),
              aP.iStopStateTls, aP.iStopStateField, aP.iStopStateValue);
    aLog.Logf(_L8("profile: overrides show_fps=%d max_fps=%d softkeys=%d/%d (%d = not set)"),
              aP.iPrefs.iShowFps, aP.iPrefs.iMaxFps, aP.iPrefs.iSoftLeftCmd, aP.iPrefs.iSoftRightCmd, KPrefUnset);
    }
