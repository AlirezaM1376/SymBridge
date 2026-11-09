// Host test of the import map lookup (the on-device build uses the real Symbian headers).
//   g++ -I hostshim -I ../src test_importmap_host.cpp ../src/importmap.cpp ../src/importmap_data.cpp -o /tmp/t && /tmp/t
#include <stdio.h>
#include <string.h>
#include "importmap.h"

static int gFail = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); gFail++; } } while (0)

int main()
    {
    CHECK(KImportMapCount > 0 && KImportMapDllCount > 0);
    // 1. table is sorted by (dll, old) with unique keys, dll indexes valid, kinds valid
    for (TInt i = 0; i < KImportMapCount; i++)
        {
        const TImportMapEntry& e = KImportMap[i];
        CHECK(e.iDll < KImportMapDllCount);
        CHECK(e.iKind <= EOverloaded);
        CHECK(e.iOld > 0);
        if (e.iKind == EDirect) CHECK(e.iNew > 0);
        if (i > 0)
            {
            const TImportMapEntry& p = KImportMap[i - 1];
            CHECK(p.iDll < e.iDll || (p.iDll == e.iDll && p.iOld < e.iOld));
            }
        }
    // 2. every entry is found by (name, ordinal), also in upper case, and returns the same row
    for (TInt i = 0; i < KImportMapCount; i++)
        {
        const TImportMapEntry& e = KImportMap[i];
        TPtrC8 n((const TText8*)KImportMapDllNames[e.iDll]);
        CHECK(ImportMapFind(n, e.iOld) == &e);
        char up[64];
        strncpy(up, KImportMapDllNames[e.iDll], sizeof(up) - 1);
        up[sizeof(up) - 1] = 0;
        for (char* c = up; *c; c++) if (*c >= 'a' && *c <= 'z') *c -= 32;
        TPtrC8 nu((const TText8*)up);
        CHECK(ImportMapFind(nu, e.iOld) == &e);
        }
    // 3. misses: unknown dll, unknown ordinal, ordinal of another dll, out of range, prefix of a dll name
    CHECK(ImportMapFind(TPtrC8((const TText8*)"nosuchdll"), 1) == NULL);
    CHECK(ImportMapFind(TPtrC8((const TText8*)"euser"), 0) == NULL);
    CHECK(ImportMapFind(TPtrC8((const TText8*)"euser"), -5) == NULL);
    CHECK(ImportMapFind(TPtrC8((const TText8*)"euser"), 70000) == NULL);
    CHECK(ImportMapFind(TPtrC8((const TText8*)"eus"), KImportMap[0].iOld) == NULL);
    CHECK(ImportMapFind(TPtrC8((const TText8*)""), 1) == NULL);
    for (TInt o = 1; o <= 0x800; o++)                    // exhaustive: Find agrees with a linear scan
        {
        for (TInt d = 0; d < KImportMapDllCount; d++)
            {
            const TImportMapEntry* lin = NULL;
            for (TInt i = 0; i < KImportMapCount; i++)
                if (KImportMap[i].iDll == d && KImportMap[i].iOld == o) lin = &KImportMap[i];
            CHECK(ImportMapFind(TPtrC8((const TText8*)KImportMapDllNames[d]), o) == lin);
            }
        }
    CHECK(strcmp((const char*)ImportMapDllName(-1), "") == 0);
    CHECK(strcmp((const char*)ImportMapDllName(KImportMapDllCount), "") == 0);
    printf("%d entries, %d dlls: %s\n", KImportMapCount, KImportMapDllCount, gFail ? "FAILED" : "all checks passed");
    return gFail ? 1 : 0;
    }
