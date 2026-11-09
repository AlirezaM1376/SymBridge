// Memory, cleanup-stack and leave shims of the old game's euser imports.
//
//  - allocation:   CBase::operator new / newL, __builtin_new / __builtin_delete (+ vec), User::AllocL,
//                  HBufC8::NewL, User::LeaveIfNull
//  - memory:       Mem::Copy / Fill / FillZ (inline in v3, so no export to bind to)
//  - cleanup/leave: CleanupStack Push/Pop with old-object deletion, User::Leave
//  - misc:         CBase::CBase(), __pure_virtual
//
// Rule for this file: a shim never throws or leaves the v3 way through old frames. Failures go through
// TOldRt::Leave(), which unwinds to the innermost OLD call frame.
// The table is added to the shim registry explicitly (MemShim_Init), owner "memshim".
#include <e32std.h>
#include <e32base.h>
#include "oldrt.h"
#include "crashwatch.h"
#include "shimreg.h"
#include "memshim.h"

static void NoteAlloc(TUint aSize, TAny* aPtr, TUint32 aLr)
    {
    if (!aPtr)
        {
        TInt big = 0;
        const TInt total = User::Available(big);
		CRUMB(16, aSize, 0, aLr);                  // a = size that failed
        CRUMB(17, big, total, aLr);                // a = largest free block, b = total free
		}
    else if (aSize >= 0x8000)
        {
        CRUMB(18, aSize, aPtr, aLr);               // big allocations that worked
		}
    }

// ---------------------------------------------------------------------------
// Shims. Called by old code with the old calling convention (identical to
// EABI for these simple signatures). They never leave the v3 way: failures are
// reported through TOldRt::Leave().
// ---------------------------------------------------------------------------
static TAny* ShimCBaseNew(TUint aSize)              // CBase::operator new(uint)
    {
    TAny* p = User::AllocZ(aSize);
    NoteAlloc(aSize, p, (TUint32)__builtin_return_address(0));
    return p;                                       // NULL on failure, like old code
    }
static TAny* ShimCBaseNewL(TUint aSize)             // CBase::newL(uint) == new (ELeave)
    {
	CRUMB_HOT(5, aSize, 0, __builtin_return_address(0));
    TAny* p = User::AllocZ(aSize);
    if (!p)
        {
        TOldRt::Leave(KErrNoMemory);
        }
    return p;
    }

static TAny* ShimCBaseCtor(TAny* aThis)             // CBase::CBase(): memory is already zeroed
    {
    return aThis;
    }

// Zeroed on purpose: old games read memory they never wrote (Qwak: the first word of its game object) and
// got zeros from the fresh old heap. It's fine on S60v3 but Belle reuses cells, so the host must hand out zeros itself.
static TAny* ShimBuiltinNew(TUint aSize)            // __builtin_new / __builtin_vec_new
    {
    TAny* p = User::AllocZ(aSize);                  // old code assumes fresh (zeroed) heap memory (Belle fix)
    NoteAlloc(aSize, p, (TUint32)__builtin_return_address(0));
    return p;
    }

static void ShimBuiltinDelete(TAny* aPtr)           // __builtin_delete / __builtin_vec_delete
    {
	CRUMB_HOT(4, aPtr, 0, __builtin_return_address(0));
    User::Free(aPtr);
    }

static void ShimPureVirtual()
    {
    User::Panic(_L("OLDGAME"), 1);
    }

static TAny* ShimMemCopy(TAny* aTrg, const TAny* aSrc, TInt aLen)
    {
    CRUMB_HOT(3, aTrg, aLen, __builtin_return_address(0));
	return Mem::Copy(aTrg, aSrc, aLen);
    }

static TAny* ShimMemFill(TAny* aTrg, TInt aLen, TUint aChar)
    {
    CRUMB_HOT(2, aTrg, aLen, __builtin_return_address(0));
	Mem::Fill(aTrg, aLen, TChar(aChar));            // returns void in the v3 SDK
    return aTrg;
    }

static TAny* ShimMemFillZ(TAny* aTrg, TInt aLen)
    {
	CRUMB_HOT(1, aTrg, aLen, __builtin_return_address(0));
    Mem::FillZ(aTrg, aLen);                         // returns void in the v3 SDK
    return aTrg;
    }

// CleanupStack shims: count what OLD code pushes, so an old-style leave
// destroys exactly those items (verified by tests T5a-d in leavetest.cpp).
// Old objects must never be destroyed through the v3 destructor path: their
// vtable is GCC 2.95 style (2 header words, first virtual = destructor at vt[2]),
// and the old destructor takes an "in-charge/delete" flag in r1 (3 = destroy + delete).

static void OldDeleteCBase(TAny* aPtr)
    {
    const TUint32* vt = *(const TUint32**)aPtr;
    typedef void (*TOldDtor)(TAny*, TInt);
    TOldThreadState& st = TOldRt::State();
    st.iInOldDelete = ETrue;
    ((TOldDtor)vt[2])(aPtr, 3);
    st.iInOldDelete = EFalse;
    }

// NOTE: no TRAP around the push. An item pushed inside a TRAP is still on the
// stack when the TRAP level exits, which panics with E32USER-CBase 71.
static void ShimPushL(CBase* aItem)                 // euser 858
    {
    CleanupStack::PushL(TCleanupItem(OldDeleteCBase, aItem));
    TOldRt::NotePush();
    }

static void ShimPop()                               // euser 828
    {
    CleanupStack::Pop();
    TOldRt::NotePop();
    }

// CleanupStack::PopAndDestroy(): destroys the item ShimPushL pushed, through the OLD destructor
// (OldDeleteCBase). The depth is lowered first so a leave inside the destructor cannot destroy it twice.
static void ShimPopAndDestroy()                     // euser 824
    {
    TOldRt::NotePop();
    CleanupStack::PopAndDestroy();
    }

static void ShimPopAndDestroyN(TInt aCount)         // CleanupStack::PopAndDestroy(int), euser 825
    {
    for (TInt i = 0; i < aCount; i++)
        {
        TOldRt::NotePop();
        }
    CleanupStack::PopAndDestroy(aCount);
    }

// euser 1627: void CleanupStack::Check(TAny* aExpectedItem). The old code pushed a raw CBase*,
// but ShimPushL pushed a TCleanupItem(OldDeleteCBase, aItem), so the v3 Check cannot compare
// it. TOldRt only counts pushes, so there is nothing to verify against: Check only verifies,
// never pops, so a no-op is safe.
static void ShimCleanupCheck(TAny* /*aExpectedItem*/)    // euser 1627
    {
    }
	
// memmove: moved to euser in v3 and has no estlib ordinal, so it cannot be DIRECT. Overlap-safe.
static TAny* ShimMemmove(TAny* aDst, const TAny* aSrc, TUint aLen)    // estlib 7
    {
    TUint8* d = (TUint8*)aDst;
    const TUint8* s = (const TUint8*)aSrc;
    if (aLen == 0 || d == s)
        {
        return aDst;
        }
    if (d + aLen <= s || s + aLen <= d)
        {
        Mem::Copy(d, s, aLen);                      // no overlap
        }
    else if (d < s)
        {
        while (aLen--) { *d++ = *s++; }
        }
    else
        {
        d += aLen;
        s += aLen;
        while (aLen--) { *--d = *--s; }
        }
    return aDst;
    }

// User::CompressAllHeaps(): Cannons calls it once at the start of ConstructL. Nothing to compress in the host.
static TInt ShimCompressAllHeaps()                  // euser 211
    {
    return 0;
    }

// Leaving v3 functions must never throw through old frames.
static TAny* ShimAllocL(TInt aSize)                 // User::AllocL(int), euser 35
    {
	CRUMB_HOT(6, aSize, 0, __builtin_return_address(0));
    TAny* p = User::AllocZ(aSize);                  // old code assumes fresh (zeroed) heap memory; Belle reuses cells
    if (!p)
        {
        TOldRt::Leave(KErrNoMemory);
        }
    return p;
    }

static TAny* ShimLeaveIfNull(TAny* aPtr)            // User::LeaveIfNull(void*), euser 628
    {
    if (!aPtr)
        {
        TOldRt::Leave(KErrNoMemory);
        }
    return aPtr;
    }

static void ShimLeave(TInt aReason)                 // User::Leave(int), euser 630
    {
    TOldRt::Leave(aReason);                         // old-style leave to the innermost old call frame; never returns
    }

static TInt ShimLeaveIfError(TInt aReason)          // User::LeaveIfError(int), euser 627
    {
    if (aReason < 0)
        {
        TOldRt::Leave(aReason);                     // old-style leave; never returns
        }
    return aReason;
    }

static TAny* ShimHBufC8NewL(TInt aMaxLength)        // HBufC8::NewL(int), euser 729
    {
    HBufC8* p = NULL;
    TInt err = KErrNone;
    TRAP(err, p = HBufC8::NewL(aMaxLength));
    if (err != KErrNone)
        {
        TOldRt::Leave(err);
        }
    return p;
    }

// ---------------------------------------------------------------------------
// CArrayFixBase with the OLD layout (RedHood: it derives its own 24 byte array class from it and reads the
// count inline at +4). The v3 class has another layout, so none of it is called: the host keeps the array in the
// game's own 24 bytes. +0 vptr (the game's), +4 count, +8 record length, +12 granularity, +16 data, +20 capacity.
// The CBufBase factory the game passes (CBufFlat::NewL) is not used: records are one flat block.
// ---------------------------------------------------------------------------
struct TOldArray
    {
    TUint32 iVtbl;
    TInt iCount;
    TInt iRecLen;
    TInt iGran;
    TUint8* iData;
    TInt iCap;
    };

// euser 1324: CArrayFixBase::CArrayFixBase(TBufRep aRep, TInt aRecordLength, TInt aGranularity); returns this
static TAny* ShimArrayCtor(TOldArray* aThis, TAny* /*aRep*/, TInt aRecLen, TInt aGran)
    {
    aThis->iCount = 0;
    aThis->iRecLen = aRecLen > 0 ? aRecLen : 1;
    aThis->iGran = aGran > 0 ? aGran : 1;
    aThis->iData = NULL;
    aThis->iCap = 0;
    return aThis;
    }

// euser 116: const TAny* CArrayFixBase::At(TInt) const
static TAny* ShimArrayAt(const TOldArray* aThis, TInt aIndex)
    {
    if (aIndex < 0 || aIndex >= aThis->iCount)
        {
        User::Panic(_L("E32USER-CBase"), 21);       // the old "index out of range" panic
        }
    return aThis->iData + aIndex * aThis->iRecLen;
    }

// euser 557: void CArrayFixBase::InsertL(TInt aIndex, const TAny* aPtr)
static void ShimArrayInsertL(TOldArray* aThis, TInt aIndex, const TAny* aPtr)
    {
    if (aIndex < 0 || aIndex > aThis->iCount)
        {
        User::Panic(_L("E32USER-CBase"), 21);
        }
    if (aThis->iCount == aThis->iCap)
        {
        const TInt newCap = aThis->iCap + aThis->iGran;
        TUint8* p = (TUint8*)User::Alloc(newCap * aThis->iRecLen);
        if (!p)
            {
            TOldRt::Leave(KErrNoMemory);
            }
        if (aThis->iData)
            {
            Mem::Copy(p, aThis->iData, aThis->iCount * aThis->iRecLen);
            User::Free(aThis->iData);
            }
        aThis->iData = p;
        aThis->iCap = newCap;
        }
    TUint8* at = aThis->iData + aIndex * aThis->iRecLen;
    for (TUint8* q = aThis->iData + aThis->iCount * aThis->iRecLen; q > at; q--)   // overlapping move to the back
        {
        q[aThis->iRecLen - 1] = q[-1];
        }
    Mem::Copy(at, aPtr, aThis->iRecLen);
    aThis->iCount++;
    }

// euser 939: void CArrayFixBase::Reset()
static void ShimArrayReset(TOldArray* aThis)
    {
    User::Free(aThis->iData);
    aThis->iData = NULL;
    aThis->iCount = 0;
    aThis->iCap = 0;
    }

// euser 1255: CArrayFixBase::~CArrayFixBase(), the destructor slot (word 2) of the game's array class; the
// old call convention is dtor(this, flags), flags & 1 = delete the object too.
static void ShimArrayDtor(TOldArray* aThis, TInt aFlags)
    {
    ShimArrayReset(aThis);
    if (aFlags & 1)
        {
        User::Free(aThis);
        }
    }

static const TShimRegEntry KMemShimEntries[] =
    {
    { "euser", 3,    (TUint32)&ShimCBaseNewL,      0, "memshim", NULL },   // CBase::newL(unsigned int)
    { "euser", 35,   (TUint32)&ShimAllocL,         0, "memshim", NULL },   // User::AllocL(int)
    { "euser", 243,  (TUint32)&ShimMemCopy,        0, "memshim", NULL },   // Mem::Copy(void*, const void*, int)
    { "euser", 411,  (TUint32)&ShimMemFillZ,       0, "memshim", NULL },   // Mem::FillZ(void*, int)
    { "euser", 416,  (TUint32)&ShimMemFill,        0, "memshim", NULL },   // Mem::Fill(void*, int, TChar)
    { "euser", 628,  (TUint32)&ShimLeaveIfNull,    0, "memshim", NULL },   // User::LeaveIfNull(void*)
    { "euser", 627,  (TUint32)&ShimLeaveIfError,   0, "memshim", NULL },   // User::LeaveIfError(int)
    { "euser", 630,  (TUint32)&ShimLeave,          0, "memshim", NULL },   // User::Leave(int)
    { "euser", 729,  (TUint32)&ShimHBufC8NewL,     0, "memshim", NULL },   // HBufC8::NewL(int)
    { "euser", 828,  (TUint32)&ShimPop,            0, "memshim", NULL },   // CleanupStack::Pop()
	{ "euser", 116,  (TUint32)&ShimArrayAt,        0, "memshim", NULL },   // CArrayFixBase::At(int) const
    { "euser", 557,  (TUint32)&ShimArrayInsertL,   0, "memshim", NULL },   // CArrayFixBase::InsertL(int, const void*)
    { "euser", 939,  (TUint32)&ShimArrayReset,     0, "memshim", NULL },   // CArrayFixBase::Reset()
    { "euser", 1255, (TUint32)&ShimArrayDtor,      0, "memshim", NULL },   // CArrayFixBase::~CArrayFixBase()
    { "euser", 1324, (TUint32)&ShimArrayCtor,      0, "memshim", NULL },   // CArrayFixBase::CArrayFixBase(TBufRep, int, int)
	{ "euser", 824,  (TUint32)&ShimPopAndDestroy,  0, "memshim", NULL },   // CleanupStack::PopAndDestroy()
    { "euser", 825,  (TUint32)&ShimPopAndDestroyN, 0, "memshim", NULL },   // CleanupStack::PopAndDestroy(int)
    { "euser", 1627, (TUint32)&ShimCleanupCheck,   0, "memshim", NULL },   // CleanupStack::PopAndDestroy(int)
	{ "euser", 211,  (TUint32)&ShimCompressAllHeaps, 0, "memshim", NULL }, // User::CompressAllHeaps()
    { "estlib", 7,   (TUint32)&ShimMemmove,        0, "memshim", NULL },   // memmove
    { "euser", 858,  (TUint32)&ShimPushL,          0, "memshim", NULL },   // CleanupStack::PushL(CBase*)
    { "euser", 1359, (TUint32)&ShimCBaseCtor,      0, "memshim", NULL },   // CBase::CBase()
    { "euser", 1504, (TUint32)&ShimBuiltinDelete,  0, "memshim", NULL },   // __builtin_delete
    { "euser", 1505, (TUint32)&ShimBuiltinNew,     0, "memshim", NULL },   // __builtin_new
    { "euser", 1506, (TUint32)&ShimBuiltinDelete,  0, "memshim", NULL },   // __builtin_vec_delete
    { "euser", 1507, (TUint32)&ShimBuiltinNew,     0, "memshim", NULL },   // __builtin_vec_new
    { "euser", 1582, (TUint32)&ShimCBaseNew,       0, "memshim", NULL },   // CBase::operator new(unsigned int)
    { "euser", 1611, (TUint32)&ShimPureVirtual,    0, "memshim", NULL }    // __pure_virtual
    };

TInt MemShim_Init()
    {
    return ShimReg_Add(KMemShimEntries, sizeof(KMemShimEntries) / sizeof(KMemShimEntries[0]));
    }

