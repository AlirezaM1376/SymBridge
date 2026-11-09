#include "shimreg.h"

const TInt KShimRegMaxTables = 16;

struct TShimRegTable
    {
    const TShimRegEntry* iEntries;
    TInt iCount;
    };

static TShimRegTable gShimRegTables[KShimRegMaxTables];
static TInt gShimRegTableCount = 0;

TInt ShimReg_Add(const TShimRegEntry* aEntries, TInt aCount)
    {
    if (!aEntries || aCount <= 0)
        {
        return KErrArgument;
        }
    if (gShimRegTableCount >= KShimRegMaxTables)
        {
        return KErrOverflow;
        }
    gShimRegTables[gShimRegTableCount].iEntries = aEntries;
    gShimRegTables[gShimRegTableCount].iCount = aCount;
    gShimRegTableCount++;
    return KErrNone;
    }

const TShimRegEntry* ShimReg_Find(const TDesC8& aDll, TInt aOld)
    {
    for (TInt table = 0; table < gShimRegTableCount; table++)
        {
        const TShimRegEntry* entries = gShimRegTables[table].iEntries;
        for (TInt i = 0; i < gShimRegTables[table].iCount; i++)
            {
            const TShimRegEntry& entry = entries[i];
            if (entry.iOld == aOld && entry.iDll && aDll.CompareF(TPtrC8((const TUint8*)entry.iDll)) == 0)
                {
                return &entry;
                }
            }
        }
    return NULL;
    }

const TShimRegEntry* ShimReg_Resolve(const TDesC8& aDll, TInt aOld, TUint32& aAddress)
    {
    aAddress = 0;
    const TShimRegEntry* entry = ShimReg_Find(aDll, aOld);
    if (!entry)
        {
        return NULL;
        }
    if (entry->iFlags & EShimRegDirect)
        {
        return entry;
        }
    if (entry->iFlags & EShimRegResolvedAtRuntime)
        {
        aAddress = entry->iResolver ? entry->iResolver() : 0;
        return entry;
        }
    aAddress = entry->iAddress;
    return entry;
    }

TInt ShimReg_CheckDuplicates(THostLog& aLog)
    {
    TInt problems = 0;
    TInt total = 0;
    for (TInt t1 = 0; t1 < gShimRegTableCount; t1++)
        {
        for (TInt i1 = 0; i1 < gShimRegTables[t1].iCount; i1++)
            {
            const TShimRegEntry& a = gShimRegTables[t1].iEntries[i1];
            total++;
            TPtrC8 aDll((const TUint8*)(a.iDll ? a.iDll : ""));
            TPtrC8 aOwner((const TUint8*)(a.iOwner ? a.iOwner : "?"));
            if (!a.iDll || (a.iAddress == 0 && !(a.iFlags & (EShimRegDirect | EShimRegResolvedAtRuntime)))
                || ((a.iFlags & EShimRegResolvedAtRuntime) && !a.iResolver))
                {
                aLog.Logf(_L8("shim registry: EMPTY entry %S %d (owner %S): no address, resolver or DIRECT flag"),
                          &aDll, a.iOld, &aOwner);
                problems++;
                }
            // later entries only: every pair is reported once
            for (TInt t2 = t1; t2 < gShimRegTableCount; t2++)
                {
                for (TInt i2 = (t2 == t1) ? i1 + 1 : 0; i2 < gShimRegTables[t2].iCount; i2++)
                    {
                    const TShimRegEntry& b = gShimRegTables[t2].iEntries[i2];
                    if (a.iOld == b.iOld && a.iDll && b.iDll
                        && aDll.CompareF(TPtrC8((const TUint8*)b.iDll)) == 0)
                        {
                        TPtrC8 bOwner((const TUint8*)(b.iOwner ? b.iOwner : "?"));
                        aLog.Logf(_L8("shim registry: DUPLICATE %S %d: %S and %S"), &aDll, a.iOld, &aOwner, &bOwner);
                        problems++;
                        }
                    }
                }
            }
        }
    aLog.Logf(_L8("shim registry: %d tables, %d entries, %d problems"), gShimRegTableCount, total, problems);
    return problems;
    }
