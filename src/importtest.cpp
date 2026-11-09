#include <e32std.h>
#include <e32base.h>
#include "importtest.h"
#include "importmap.h"

// Old euser ordinals used for spot checks; the current v3 ordinals come from importmap_data.cpp.
static const TInt KOldTickCount = 1137;  // User::TickCount()
static const TInt KOldAppend8   = 85;    // TDes8::Append(TChar)
static const TInt KOldAllocL    = 35;    // User::AllocL(int)

typedef TUint (*TTickFn)();
typedef TAny* (*TAppendFn)(TDes8* aThis, TUint aChar);
typedef TAny* (*TAllocLFn)(TInt aSize);

static inline const TImportMapEntry* FindEntry(const TDesC8& aDll, TInt aOld)
    {
    return ImportMapFind(aDll, aOld);
    }

static TInt DirectOrdinal(THostLog& aLog, TInt aOld, const TDesC8& aName)
    {
    const TImportMapEntry* entry = FindEntry(_L8("euser"), aOld);
    if (!entry || entry->iKind != EDirect || entry->iNew == 0)
        {
        aLog.Logf(_L8("%S: old euser ordinal %d is not a direct map entry"), &aName, aOld);
        return 0;
        }
    return (TInt)entry->iNew;
    }

static void SpotChecks(THostLog& aLog)
    {
    aLog.Text(_L8("--- spot checks: call functions through the mapped ordinals ---"));
    RLibrary eu;
    TInt err = eu.Load(_L("euser.dll"));
    if (err != KErrNone)
        {
        aLog.Logf(_L8("cannot load euser.dll: %d"), err);
        return;
        }

    const TInt tickOrdinal = DirectOrdinal(aLog, KOldTickCount, _L8("TickCount"));
    TTickFn tick = tickOrdinal ? (TTickFn)eu.Lookup(tickOrdinal) : NULL;
    if (tick)
        {
        TUint t1 = tick();
        User::After(200000);
        TUint t2 = tick();
        aLog.Logf(_L8("TickCount via ordinal: %u -> %u after 200 ms (SDK direct: %u)"),
                  t1, t2, User::TickCount());
        }
    else
        {
        aLog.Text(_L8("TickCount ordinal returned NULL"));
        }

    const TInt appendOrdinal = DirectOrdinal(aLog, KOldAppend8, _L8("Append"));
    TAppendFn app = appendOrdinal ? (TAppendFn)eu.Lookup(appendOrdinal) : NULL;
    if (app)
        {
        TBuf8<8> b;
        app(&b, 'A');
        app(&b, 'B');
        aLog.Logf(_L8("TDes8::Append(TChar) via ordinal: length=%d (expect 2), chars=%d,%d (expect 65,66)"),
                  b.Length(), b.Length() > 0 ? b[0] : 0, b.Length() > 1 ? b[1] : 0);
        }
    else
        {
        aLog.Text(_L8("Append ordinal returned NULL"));
        }

    const TInt allocOrdinal = DirectOrdinal(aLog, KOldAllocL, _L8("AllocL"));
    TAllocLFn al = allocOrdinal ? (TAllocLFn)eu.Lookup(allocOrdinal) : NULL;
    if (al)
        {
        TAny* p = NULL;
        TInt e = KErrNone;
        TRAP(e, p = al(64));
        aLog.Logf(_L8("User::AllocL(64) via ordinal -> ptr=%08x err=%d (expect non-zero, 0)"), (TUint32)p, e);
        User::Free(p);
        p = NULL;
        TRAP(e, p = al(0x4000000));
        aLog.Logf(_L8("User::AllocL(64MB) via ordinal -> err=%d (expect -4)"), e);
        User::Free(p);
        }
    else
        {
        aLog.Text(_L8("AllocL ordinal returned NULL"));
        }
    eu.Close();
    }

void RunImportMapTest(THostLog& aLog, const CLegacyImage& aImg)
    {
    aLog.Text(_L8("--- import map test (resolve on this phone) ---"));
    TInt kindCount[EOverloaded + 1] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    TInt unmapped = 0;
    TInt totalOk = 0;
    TInt totalNull = 0;
    TInt ambiguousOk = 0;
    TInt ambiguousNull = 0;

    for (TInt b = 0; b < aImg.iBlocks.Count(); b++)
        {
        TPtrC8 full = aImg.DllName(b);
        TInt br = full.Locate('[');
        TPtrC8 base8 = (br >= 0) ? full.Left(br) : full;
        TBuf8<48> lower8;
        lower8.Copy(base8);
        lower8.LowerCase();
        TBuf<64> name;
        name.Copy(lower8);
        name.Append(_L(".dll"));

        RLibrary lib;
        TInt err = lib.Load(name);
        if (err != KErrNone)
            {
            aLog.Logf(_L8("  %S: cannot load (%d)"), &lower8, err);
            continue;
            }

        const TLegacyImportBlock& blk = aImg.iBlocks[b];
        TInt ok = 0;
        TInt nul = 0;
        for (TInt k = 0; k < blk.iCount; k++)
            {
            TInt old = (TInt)aImg.iOrdinals[blk.iFirst + k];
            const TImportMapEntry* e = FindEntry(lower8, old);
            if (!e)
                {
                unmapped++;
                continue;
                }
            if (e->iKind > EOverloaded)
                {
                aLog.Logf(_L8("    invalid map kind: %S old=%d kind=%d"), &lower8, old, (TInt)e->iKind);
                continue;
                }
            kindCount[e->iKind]++;
            if (e->iKind == EDirect || e->iKind == EAmbiguous)
                {
                if (lib.Lookup(e->iNew) != NULL)
                    {
                    if (e->iKind == EAmbiguous)
                        {
                        ambiguousOk++;
                        }
                    else
                        {
                        ok++;
                        }
                    }
                else
                    {
                    if (e->iKind == EAmbiguous)
                        {
                        ambiguousNull++;
                        aLog.Logf(_L8("    AMBIGUOUS NULL: %S old=%d new=%d"), &lower8, old, (TInt)e->iNew);
                        }
                    else
                        {
                        nul++;
                        aLog.Logf(_L8("    NULL: %S old=%d new=%d"), &lower8, old, (TInt)e->iNew);
                        }
                    }
                }
            }
        aLog.Logf(_L8("  %S: lookups ok=%d NULL=%d (imports=%d)"), &lower8, ok, nul, blk.iCount);
        totalOk += ok;
        totalNull += nul;
        lib.Close();
        }

    aLog.Logf(_L8("total lookups ok=%d NULL=%d unmapped=%d"), totalOk, totalNull, unmapped);
    aLog.Logf(_L8("ambiguous lookup-only: ok=%d NULL=%d"), ambiguousOk, ambiguousNull);
    aLog.Logf(_L8("kinds: direct=%d shim=%d vslot=%d helper=%d nolib=%d ambiguous=%d sigchg=%d"),
              kindCount[0], kindCount[1], kindCount[2], kindCount[3],
              kindCount[4], kindCount[5], kindCount[6]);
    aLog.Logf(_L8("kinds: overloaded=%d (not probed; no ordinal selected)"), kindCount[EOverloaded]);

    SpotChecks(aLog);
    }
