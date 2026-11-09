#include "importmap.h"

const TText8* ImportMapDllName(TInt aIndex)
    {
    if (aIndex < 0 || aIndex >= KImportMapDllCount)
        {
        return (const TText8*)"";
        }
    return (const TText8*)KImportMapDllNames[aIndex];
    }

const TImportMapEntry* ImportMapFind(const TDesC8& aDll, TInt aOld)
    {
    TInt dll = -1;
    for (TInt i = 0; i < KImportMapDllCount; i++)           // a few dozen names at most
        {
        TPtrC8 name(ImportMapDllName(i));
        if (name.CompareF(aDll) == 0)
            {
            dll = i;
            break;
            }
        }
    if (dll < 0 || aOld < 0 || aOld > 0xFFFF)
        {
        return NULL;
        }
    TInt lo = 0;
    TInt hi = KImportMapCount - 1;
    while (lo <= hi)                                        // table is sorted by (iDll, iOld)
        {
        const TInt mid = (lo + hi) >> 1;
        const TImportMapEntry& e = KImportMap[mid];
        if (e.iDll < dll || (e.iDll == dll && e.iOld < aOld))
            {
            lo = mid + 1;
            }
        else if (e.iDll == dll && e.iOld == aOld)
            {
            return &e;
            }
        else
            {
            hi = mid - 1;
            }
        }
    return NULL;
    }
