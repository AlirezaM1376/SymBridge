#include <e32base.h>
#include "cfgtext.h"

const TInt KMaxCfgFileSize = 8192;

HBufC8* CfgText_ReadFileL(RFs& aFs, const TDesC& aPath)
    {
    RFile f;
    User::LeaveIfError(f.Open(aFs, aPath, EFileRead | EFileShareAny));
    CleanupClosePushL(f);
    TInt size = 0;
    User::LeaveIfError(f.Size(size));
    if (size > KMaxCfgFileSize)
        {
        User::Leave(KErrTooBig);
        }
    HBufC8* buf = HBufC8::NewLC(size);
    TPtr8 p = buf->Des();
    User::LeaveIfError(f.Read(p));
    CleanupStack::Pop(buf);
    CleanupStack::PopAndDestroy();          // f
    return buf;
    }

TPtrC8 CfgText_Trim(const TDesC8& a)
    {
    TInt s = 0;
    TInt e = a.Length();
    while (s < e && (a[s] == ' ' || a[s] == '\t' || a[s] == '\r'))
        {
        s++;
        }
    while (e > s && (a[e - 1] == ' ' || a[e - 1] == '\t' || a[e - 1] == '\r'))
        {
        e--;
        }
    return a.Mid(s, e - s);
    }

TBool CfgText_NextLine(TPtrC8& aRest, TPtrC8& aLine)
    {
    if (aRest.Length() == 0)
        {
        return EFalse;
        }
    const TInt nl = aRest.Locate('\n');
    aLine.Set(nl >= 0 ? aRest.Left(nl) : aRest);
    aRest.Set(nl >= 0 ? aRest.Mid(nl + 1) : TPtrC8());
    return ETrue;
    }

TBool CfgText_SplitLine(const TDesC8& aLine, TPtrC8& aName, TPtrC8& aValue)
    {
    TPtrC8 line(CfgText_Trim(aLine));
    if (line.Length() == 0 || line[0] == '#')
        {
        return EFalse;
        }
    const TInt eq = line.Locate('=');
    if (eq <= 0)
        {
        return EFalse;
        }
    aName.Set(CfgText_Trim(line.Left(eq)));
    TPtrC8 v(line.Mid(eq + 1));
    const TInt hash = v.Locate('#');
    if (hash >= 0)
        {
        v.Set(v.Left(hash));
        }
    aValue.Set(CfgText_Trim(v));
    return ETrue;
    }

TBool CfgText_ParseNumber(const TDesC8& aValue, TInt& aOut)
    {
    TLex8 lex(aValue);
    if (aValue.Length() > 2 && aValue[0] == '0' && (aValue[1] == 'x' || aValue[1] == 'X'))
        {
        lex.Inc(2);
        TUint32 u = 0;
        if (lex.Val(u, EHex) == KErrNone)
            {
            aOut = (TInt)u;
            return ETrue;
            }
        return EFalse;
        }
    TInt v = 0;
    if (lex.Val(v) == KErrNone)
        {
        aOut = v;
        return ETrue;
        }
    return EFalse;
    }
