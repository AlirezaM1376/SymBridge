#include <e32base.h>
#include <f32file.h>
#include "gamelocator.h"
#include "cfgtext.h"

_LIT(KRootDir, "\\symbridge\\");
_LIT(KAutostartName, "autostart");
_LIT8(KWhyAuto, "autostart");
_LIT8(KWhyFirst, "first folder, no matching autostart");
_LIT8(KWhyOnly, "only game");

const TUint32 KUidAppImage = 0x100039ce;           // UID2 of a pre-9 application image
const TUint32 KEpocSignature = 0x434f5045;         // 'EPOC' at offset 0x10
static const TText KDriveOrder[] = { 'E', 'F', 'C' };

static TUint32 U32(const TDesC8& aD, TInt aOff)
    {
    return (TUint32)aD[aOff] | ((TUint32)aD[aOff + 1] << 8) | ((TUint32)aD[aOff + 2] << 16) | ((TUint32)aD[aOff + 3] << 24);
    }

// An application image: 'EPOC' signature and UID2 = application. Returns UID3 and the code size.
static TBool ReadHeader(RFs& aFs, const TDesC& aPath, TUint32& aUid3, TInt& aCodeSize)
    {
    RFile f;
    if (f.Open(aFs, aPath, EFileRead | EFileShareAny) != KErrNone)
        {
        return EFalse;
        }
    TBuf8<0x80> h;
    const TInt err = f.Read(0, h, 0x80);
    f.Close();
    if (err != KErrNone || h.Length() < 0x34)
        {
        return EFalse;
        }
    if (U32(h, 0x10) != KEpocSignature || U32(h, 4) != KUidAppImage)
        {
        return EFalse;
        }
    aUid3 = U32(h, 8);
    aCodeSize = (TInt)U32(h, 0x30);
    return ETrue;
    }

// \symbridge\autostart: first non-empty, non-# line = folder name or UID3 (exactly 8 hex digits).
static void ReadAutostart(RFs& aFs, const TDesC& aRoot, TBuf<64>& aName, TUint32& aUid)
    {
    aName.Zero();
    aUid = 0;
    TFileName p(aRoot);
    p.Append(KAutostartName);
    RFile f;
    if (f.Open(aFs, p, EFileRead | EFileShareAny) != KErrNone)
        {
        return;
        }
    TBuf8<512> buf;
    f.Read(buf);
    f.Close();
    TPtrC8 rest(buf);
    TPtrC8 rawLine;
    while (CfgText_NextLine(rest, rawLine))
        {
        TPtrC8 line(CfgText_Trim(rawLine));
        if (line.Length() == 0 || line[0] == '#')
            {
            continue;
            }
        aName.Copy(line.Left(aName.MaxLength()));
        if (line.Length() == 8)
            {
            TBool hex = ETrue;
            for (TInt i = 0; i < 8; i++)
                {
                if (!TChar(line[i]).IsHexDigit())
                    {
                    hex = EFalse;
                    }
                }
            if (hex)
                {
                TLex8 lex(line);
                TUint32 u = 0;
                if (lex.Val(u, EHex) == KErrNone)
                    {
                    aUid = u;
                    }
                }
            }
        return;
        }
    }

TInt GameLocator_Find(RFs& aFs, THostLog& aLog, TGameInfo& aOut)
    {
    for (TInt i = 0; i < 3; i++)
        {
        TChar letter(KDriveOrder[i]);
        TInt drv = 0;
        if (RFs::CharToDrive(letter, drv) != KErrNone)
            {
            continue;
            }
        TDriveInfo info;
        if (aFs.Drive(info, drv) != KErrNone || info.iType == EMediaNotPresent)
            {
            aLog.Logf(_L8("locator: drive %c: not present, skipped"), (TInt)KDriveOrder[i]);
            continue;
            }
        TFileName root;
        root.Append(letter);
        root.Append(_L(":"));
        root.Append(KRootDir);
        CDir* top = NULL;
        const TInt terr = aFs.GetDir(root, KEntryAttDir, ESortByName, top);
        if (terr != KErrNone || !top)
            {
            aLog.Logf(_L8("locator: no \\symbridge on %c: (%d)"), (TInt)KDriveOrder[i], terr);
            delete top;
            continue;
            }
        TBuf<64> autoName;
        TUint32 autoUid = 0;
        ReadAutostart(aFs, root, autoName, autoUid);
        TInt games = 0;
        TBool matched = EFalse;
        for (TInt k = 0; k < top->Count() && !matched; k++)
            {
            const TEntry& de = (*top)[k];
            if (!de.IsDir())
                {
                continue;
                }
            TFileName sub(root);
            sub.Append(de.iName);
            sub.Append(_L("\\"));
            TFileName pattern(sub);
            pattern.Append(_L("*.app"));
            CDir* files = NULL;
            if (aFs.GetDir(pattern, KEntryAttNormal, ESortByName, files) != KErrNone || !files)
                {
                delete files;
                continue;
                }
            for (TInt m = 0; m < files->Count(); m++)
                {
                TFileName path(sub);
                path.Append((*files)[m].iName);
                TUint32 uid3 = 0;
                TInt codeSize = 0;
                if (!ReadHeader(aFs, path, uid3, codeSize))
                    {
                    continue;
                    }
                games++;
                const TBool wanted = (autoUid != 0 && uid3 == autoUid) ||
                                     (autoName.Length() > 0 && autoName.CompareF(de.iName) == 0);
                if (wanted || games == 1)
                    {
                    aOut.iAppPath.Copy(path);
                    aOut.iRoot.Copy(root);
                    aOut.iUid3 = uid3;
                    aOut.iCodeSize = codeSize;
                    matched = wanted;
                    }
                break;                                  // one game per folder
                }
            delete files;
            }
        delete top;
        if (games > 0)
            {
            TBuf8<160> shown;
            shown.Copy(aOut.iAppPath.Left(shown.MaxLength()));
            const TDesC8* why = matched ? &KWhyAuto() : (games > 1 ? &KWhyFirst() : &KWhyOnly());
            aLog.Logf(_L8("locator: %c:\\symbridge has %d game(s); chosen '%S' uid3=%08x (%S)"),
                      (TInt)KDriveOrder[i], games, &shown, aOut.iUid3, why);
            return KErrNone;
            }
        aLog.Logf(_L8("locator: %c:\\symbridge has no game"), (TInt)KDriveOrder[i]);
        }

    return KErrNotFound;
    }
