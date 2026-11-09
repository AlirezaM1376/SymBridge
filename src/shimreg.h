// Explicitly assembled registry of legacy import handling policies.
// Subsystems provide immutable entry arrays and add them during ordered startup.
#ifndef SHIMREG_H
#define SHIMREG_H

#include <e32std.h>
#include "hostlog.h"

enum TShimRegFlag
    {
    EShimRegDirect = 0x01,             // bind through the v3 ordinal in the import map
    EShimRegResolvedAtRuntime = 0x02  // call iResolver to obtain the shim address
    };

typedef TUint32 (*TShimRegResolver)();

struct TShimRegEntry
    {
    const char* iDll;                // lower-case DLL name, no ".dll"
    TInt iOld;                        // S60v2 import ordinal
    TUint32 iAddress;                 // shim function address; zero for DIRECT or runtime-resolved entries
    TUint32 iFlags;                   // EShimRegDirect and/or EShimRegResolvedAtRuntime
    const char* iOwner;               // subsystem or shim owner name
    TShimRegResolver iResolver;       // non-NULL when EShimRegResolvedAtRuntime is set
    };

// Add one subsystem's static table explicitly during startup; no static constructors are used.
TInt ShimReg_Add(const TShimRegEntry* aEntries, TInt aCount);

// Find the unique registered policy for a lower-case DLL name and old ordinal; NULL if absent.
const TShimRegEntry* ShimReg_Find(const TDesC8& aDll, TInt aOld);

// The ONE lookup the linker uses. Finds the registered policy and resolves it:
//   returns NULL                      : not registered (the import becomes a trap stub)
//   returns an entry, aAddress != 0   : shim function (plain or runtime-resolved)
//   returns an entry with EShimRegDirect : bind to the real v3 function through the import map (aAddress = 0)
//   returns an entry, aAddress == 0 and not DIRECT : a runtime-resolved entry whose resolver gave no address
//                                       yet (the import becomes a trap stub)
const TShimRegEntry* ShimReg_Resolve(const TDesC8& aDll, TInt aOld, TUint32& aAddress);

// Start-up check of everything registered: logs every (dll, ordinal) that two entries claim, and every entry
// that does nothing (no address, no resolver, not DIRECT). Logs one summary line. Returns the number of
// problems found; the linker refuses to link when it is not 0.
TInt ShimReg_CheckDuplicates(THostLog& aLog);

#endif
