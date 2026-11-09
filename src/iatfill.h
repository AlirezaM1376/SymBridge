// Fills the import address table (IAT) of the relocated game image.
//
// Every import of the game gets exactly one of:
//   direct : address of the real S60v3 function (only the ones listed in directbind.cpp)
//   shim   : address of one of our own replacement functions (see shimreg.h)
//   trap   : address of a tiny stub that records WHICH import was called. In trace mode (the normal
//            mode, see SetTrace) the stub logs the first calls and returns 0, so the game goes on; otherwise
//            it stops the run. The trap stubs make every unimplemented import show up in the log.
#ifndef IATFILL_H
#define IATFILL_H

#include <e32std.h>
#include "hostlog.h"
#include "legacyimage.h"

const TInt KMaxLinkLibs = 24;
const TInt KIatStubSize = 52;             // bytes per trap/trace stub (13 words, see MakeStub)
// Upper limit of imports per game image. The stub carries a 16-bit index, so this is only a
// sizing constant for the per-import counters; raise it freely (up to 65535).
const TInt KMaxLinkImports = 1024;
const TInt KIatStubHitReason = -9999;     // reason passed to TOldRt::Leave by a trap stub

class TIatLinker
    {
public:
    TIatLinker() : iDirect(0), iShim(0), iTrap(0), iLibCount(0) {}

    // Trace mode: unresolved imports are logged (index, old ordinal, first 3 args; only the first hits of
    // each import) and return (this for ctor-like imports, else 0) instead of stopping the run.
    static void SetTrace(THostLog* aLog, const CLegacyImage* aImg, TBool aOn);

    // Logs every import that was reached through a stub, with its hit count (soft mode counters).
    static void DumpHits(THostLog& aLog, const CLegacyImage& aImg);
    // Unique imports reached by trap stubs; used for the final host summary line.
    static TInt ReachedCount();

    // aBase  : start of the relocated code region (image already copied + relocated)
    // aStubs : free, writable+executable area with room for KIatStubSize bytes per import
    // The caller must call User::IMB_Range over both areas afterwards.
    TInt Link(THostLog& aLog, const CLegacyImage& aImg, TUint8* aBase, TUint8* aStubs,
              THostLog* aReportLog = NULL);
    void Close();

    // Index (0..KMaxLinkImports-1) of the import whose trap stub fired last, or -1.
    static TInt LastHit();
    // Human readable description of import number aIndex.
    static void Describe(const CLegacyImage& aImg, TInt aIndex, TDes8& aOut);

public:
    TInt iDirect;
    TInt iShim;
    TInt iTrap;

private:
    RLibrary iLibs[KMaxLinkLibs];
    TInt iLibCount;
    };

#endif
