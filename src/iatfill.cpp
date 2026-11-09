#include <e32std.h>
#include <e32base.h>
#include "iatfill.h"
#include "oldrt.h"
#include "importmap.h"
#include "shimreg.h"

// ---------------------------------------------------------------------------
// Trap stubs
// ---------------------------------------------------------------------------

// Trace mode state. An import without a shim does not stop the game: its first hits are logged, the call
// returns 0 (or "this" for constructor-like imports) and the game goes on.
static TBool gTraceMode = EFalse;
static THostLog* gTraceLog = NULL;
static const CLegacyImage* gTraceImg = NULL;
static TInt gTraceCount = 0;
static TInt gTraceHits[KMaxLinkImports];             // per import: only the first KTraceLogPerImport hits are logged
static TUint8 gTraceKind[KMaxLinkImports];           // per import: 0 not looked up yet, 1 plain, 2 returns 'this'
const TInt KTraceLogPerImport = 12;
const TInt KTraceHardLimit = 2000000000;             // only a runaway guard

void TIatLinker::SetTrace(THostLog* aLog, const CLegacyImage* aImg, TBool aOn)
    {
    gTraceLog = aLog;
    gTraceImg = aImg;
    gTraceMode = aOn;
    gTraceCount = 0;
    Mem::FillZ(gTraceHits, sizeof(gTraceHits));
    Mem::FillZ(gTraceKind, sizeof(gTraceKind));
    }

static const TImportMapEntry* EntryAt(const CLegacyImage& aImg, TInt aIndex, TDes8& aDll, TInt& aOld);

// Called from a stub with the first three arguments of the old call.
// Normal mode: stop the run (never returns). Trace mode: log and return.
static TInt TrapHandler(TInt aIdx, TInt a0, TInt a1, TInt a2)
    {
    TOldThreadState& st = TOldRt::State();
    st.iHit = aIdx;
    if (st.iInOldDelete)
        {
        // An unresolved import was reached from an old destructor while a leave is being
        // cleaned up. Leaving again would jump to the wrong trap frame: log and ignore.
        if (gTraceLog)
            {
            gTraceLog->Logf(_L8("UNWIND: stub #%d hit inside old destructor, ignored"), aIdx);
            }
        return 0;
        }
    if (!gTraceMode || !gTraceLog || !gTraceImg)
        {
        TOldRt::Abandon(KIatStubHitReason);
        return 0;
        }
    if (++gTraceCount > KTraceHardLimit)
        {
        gTraceLog->Text(_L8("TRACE: limit reached, stopping"));
        TOldRt::Abandon(KIatStubHitReason);
        return 0;
        }
    // A busy import can be hit thousands of times a second: after its first hits only counters are touched.
    // The import map is searched once per import (and for the log lines), not on every call.
    const TBool inRange = (aIdx >= 0 && aIdx < KMaxLinkImports);
    const TInt hits = inRange ? ++gTraceHits[aIdx] : 1;
    const TBool logIt = (hits <= KTraceLogPerImport);
    TBool returnsThis = EFalse;                         // ctor-like imports (ambiguous kind) return 'this'
    if (inRange && gTraceKind[aIdx] != 0)
        {
        returnsThis = (gTraceKind[aIdx] == 2);
        }
    if (logIt || !inRange || gTraceKind[aIdx] == 0)
        {
        TBuf8<48> dll;
        TInt old = 0;
        const TImportMapEntry* e = EntryAt(*gTraceImg, aIdx, dll, old);
        returnsThis = (e && e->iKind == EAmbiguous);
        if (inRange)
            {
            gTraceKind[aIdx] = returnsThis ? 2 : 1;
            }
        if (logIt)
            {
            gTraceLog->Logf(_L8("TRACE #%d %S %d  args=%08x %08x %08x"), aIdx, &dll, old, a0, a1, a2);
            if (hits == KTraceLogPerImport)
                {
                gTraceLog->Logf(_L8("TRACE #%d: further hits are counted but not logged"), aIdx);
                }
            }
        }
    return returnsThis ? a0 : 0;
    }

// 13 words per import (KIatStubSize = 52). Saves r4/lr, aligns sp, shifts r0-r2 to r1-r3, passes
// idx in r0, calls TrapHandler and returns its result to the old caller:
//   push {r4,lr} ; mov r4,sp ; bic sp,sp,#7 ; mov r3,r2 ; mov r2,r1 ; mov r1,r0
//   mov r0,#idx_lo ; orr r0,r0,#(idx_hi << 8) ; ldr ip,[pc,#8] ; blx ip
//   mov sp,r4 ; pop {r4,pc} ; .word TrapHandler
// An ARM immediate is an 8-bit value rotated right by an even amount, so a plain "mov r0,#idx" only
// works up to 255. Two instructions give a 16-bit index without needing ARMv6T2 (movw): the low byte
// goes in with mov, the high byte with orr using rotate field 12 (ror 24, i.e. value << 8).
static TUint32 MakeStub(TUint8* aStubs, TInt aIdx)
    {
    TUint32* s = (TUint32*)(aStubs + KIatStubSize * aIdx);
    s[0]  = 0xE92D4010;
    s[1]  = 0xE1A0400D;
    s[2]  = 0xE3CDD007;
    s[3]  = 0xE1A03002;
    s[4]  = 0xE1A02001;
    s[5]  = 0xE1A01000;
    s[6]  = 0xE3A00000 | ((TUint32)aIdx & 0xFF);            // mov r0,#idx_lo
    s[7]  = 0xE3800C00 | (((TUint32)aIdx >> 8) & 0xFF);     // orr r0,r0,#idx_hi,ror 24
    s[8]  = 0xE59FC008;                                     // ldr ip,[pc,#8]  -> s[12]
    s[9]  = 0xE12FFF3C;                                     // blx ip
    s[10] = 0xE1A0D004;                                     // mov sp,r4
    s[11] = 0xE8BD8010;                                     // pop {r4,pc}
    s[12] = (TUint32)&TrapHandler;
    return (TUint32)s;
    }

// ---------------------------------------------------------------------------
// Map helpers
// ---------------------------------------------------------------------------
static void BaseName(const TDesC8& aFull, TDes8& aOut)
    {
    TInt br = aFull.Locate('[');
    TInt len = (br >= 0) ? br : aFull.Length();
    if (len > aOut.MaxLength())
        {
        len = aOut.MaxLength();
        }
    aOut.Copy(aFull.Left(len));
    aOut.LowerCase();
    }

// ---------------------------------------------------------------------------
// TIatLinker
// ---------------------------------------------------------------------------
TInt TIatLinker::LastHit()
    {
    return TOldRt::State().iHit;
    }

void TIatLinker::Close()
    {
    for (TInt i = 0; i < iLibCount; i++)
        {
        iLibs[i].Close();
        }
    iLibCount = 0;
    }

static const TImportMapEntry* EntryAt(const CLegacyImage& aImg, TInt aIndex, TDes8& aDll, TInt& aOld)
    {
    aDll.Zero();
    aOld = 0;
    for (TInt b = 0; b < aImg.iBlocks.Count(); b++)
        {
        const TLegacyImportBlock& blk = aImg.iBlocks[b];
        if (aIndex >= blk.iFirst && aIndex < blk.iFirst + blk.iCount)
            {
            BaseName(aImg.DllName(b), aDll);
            aOld = (TInt)aImg.iOrdinals[aIndex];
            return ImportMapFind(aDll, aOld);
            }
        }
    return NULL;
    }

void TIatLinker::Describe(const CLegacyImage& aImg, TInt aIndex, TDes8& aOut)
    {
    TBuf8<48> dll;
    TInt old = 0;
    const TImportMapEntry* e = EntryAt(aImg, aIndex, dll, old);
    if (e)
        {
        aOut.Format(_L8("%S old=%d new=%d kind=%d"), &dll, old, (TInt)e->iNew, (TInt)e->iKind);
        }
    else
        {
        aOut.Format(_L8("%S old=%d (not in map)"), &dll, old);
        }
    }

TInt TIatLinker::Link(THostLog& aLog, const CLegacyImage& aImg, TUint8* aBase, TUint8* aStubs,
                      THostLog* aReportLog)
    {
    const TInt total = aImg.iOrdinals.Count();
    if (total > KMaxLinkImports)
        {
        aLog.Logf(_L8("link: %d imports, more than KMaxLinkImports (%d)"), total, KMaxLinkImports);
        return KErrNotSupported;
        }
    // Everything the game may be linked to is in the shim registry. Two claims on one import, or an entry that
    // does nothing, are errors: refuse to link instead of letting table order pick a winner.
    if (ShimReg_CheckDuplicates(aLog) != 0)
        {
        return KErrAlreadyExists;
        }
    TUint32* iat = (TUint32*)(aBase + aImg.iTextSize);

    // Sanity: the IAT in the code region must still hold the old ordinals.
    for (TInt i = 0; i < total; i++)
        {
        if (iat[i] != aImg.iOrdinals[i])
            {
            aLog.Logf(_L8("link: IAT slot %d holds %x, expected ordinal %d"),
                      i, iat[i], (TInt)aImg.iOrdinals[i]);
            return KErrCorrupt;
            }
        }
    aLog.Logf(_L8("link: IAT sanity ok, %d slots match the import ordinals"), total);

    TUint8 trapped[KMaxLinkImports];
    Mem::FillZ(trapped, sizeof(trapped));
    TInt idx = 0;
    for (TInt b = 0; b < aImg.iBlocks.Count(); b++)
        {
        const TLegacyImportBlock& blk = aImg.iBlocks[b];
        TBuf8<48> dll;
        BaseName(aImg.DllName(b), dll);

        RLibrary* lib = NULL;           // loaded lazily, only if a direct import needs it
        TBool loadFailed = EFalse;

        for (TInt k = 0; k < blk.iCount; k++, idx++)
            {
            const TInt old = (TInt)aImg.iOrdinals[idx];
            const TImportMapEntry* e = ImportMapFind(dll, old);
            TUint32 target = 0;

            // The ONE lookup: the shim registry decides (shim, DIRECT, or not registered = trap stub).
            TUint32 shim = 0;
            const TShimRegEntry* reg = ShimReg_Resolve(dll, old, shim);
            const TShimRegEntry* direct = (reg && (reg->iFlags & EShimRegDirect)) ? reg : NULL;
            if (direct && !(e && e->iKind == EDirect && e->iNew != 0))
                {
                // Listed as DIRECT but the import map has no exact v3 match: a list error, never a silent bind.
                aLog.Logf(_L8("link: DIRECT entry %S %d has no usable import map row (kind %d) -> trap"),
                          &dll, old, e ? (TInt)e->iKind : -1);
                }
            else if (reg && !direct && shim == 0)
                {
                // Runtime-resolved entry that has no address (yet): the import gets a trap stub.
                TPtrC8 owner((const TUint8*)reg->iOwner);
                aLog.Logf(_L8("link: %S %d (owner %S) has no address at link time -> trap"), &dll, old, &owner);
                }
            if (shim != 0)
                {
                target = shim;
                iShim++;
                }
            else if (direct && e && e->iKind == EDirect && e->iNew != 0)
                {
                if (!lib && !loadFailed)
                    {
                    TBuf<64> name;
                    name.Copy(dll);
                    name.Append(_L(".dll"));
                    if (iLibCount < KMaxLinkLibs && iLibs[iLibCount].Load(name) == KErrNone)
                        {
                        lib = &iLibs[iLibCount++];
                        }
                    else
                        {
                        loadFailed = ETrue;
                        aLog.Logf(_L8("link: cannot load %S.dll"), &dll);
                        }
                    }
                if (lib)
                    {
                    target = (TUint32)lib->Lookup(e->iNew);
                    if (target != 0)
                        {
                        iDirect++;
                        }
                    }
                }

            if (target == 0)
                {
                target = MakeStub(aStubs, idx);
                iTrap++;
                trapped[idx] = 1;
                }
            if (aReportLog)
                {
                // "<kind> <owner>": shim memshim / direct directbind / trap
                TBuf8<48> kind;
                if (shim != 0)
                    {
                    kind.Copy(_L8("shim "));
                    kind.Append(TPtrC8((const TUint8*)reg->iOwner));
                    }
                else if (trapped[idx])
                    {
                    kind.Copy(_L8("trap"));
                    }
                else
                    {
                    kind.Copy(_L8("direct "));
                    kind.Append(TPtrC8((const TUint8*)reg->iOwner));
                    }
                aReportLog->Logf(_L8("#%d %S %d %S"), idx, &dll, old, &kind);
                }
            iat[idx] = target;
            }
        }

    aLog.Logf(_L8("link: direct=%d shim=%d trap=%d (total %d)"), iDirect, iShim, iTrap, idx);

    // Compact list of what is still a trap/trace stub, per DLL (old ordinals; names are in the CSV).
    idx = 0;
    for (TInt b = 0; b < aImg.iBlocks.Count(); b++)
        {
        const TLegacyImportBlock& blk = aImg.iBlocks[b];
        TBuf8<48> dll;
        BaseName(aImg.DllName(b), dll);
        TBuf8<200> line;
        TInt n = 0;
        for (TInt k = 0; k < blk.iCount; k++, idx++)
            {
            if (!trapped[idx])
                {
                continue;
                }
            n++;
            if (line.Length() > 150)
                {
                aLog.Logf(_L8("link: trap %S: %S"), &dll, &line);
                line.Zero();
                }
            line.AppendNum((TInt)aImg.iOrdinals[idx]);
            line.Append(' ');
            }
        if (n > 0)
            {
            aLog.Logf(_L8("link: trap %S (%d): %S"), &dll, n, &line);
            }
        }
    return KErrNone;
    }

TInt TIatLinker::ReachedCount()
    {
    TInt reached = 0;
    for (TInt i = 0; i < KMaxLinkImports; i++)
        {
        if (gTraceHits[i] > 0)
            {
            reached++;
            }
        }
    return reached;
    }

void TIatLinker::DumpHits(THostLog& aLog, const CLegacyImage& aImg)
    {
    TInt shown = 0;
    for (TInt i = 0; i < KMaxLinkImports; i++)
        {
        if (gTraceHits[i] > 0)
            {
            TBuf8<48> dll;
            TInt old = 0;
            EntryAt(aImg, i, dll, old);
            aLog.Logf(_L8("hits #%d %S %d = %d"), i, &dll, old, gTraceHits[i]);
            shown++;
            }
        }
    aLog.Logf(_L8("hits: %d unresolved imports were reached"), shown);
    }
