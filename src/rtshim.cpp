#include <e32base.h>
#include <e32math.h>
#include "rtshim.h"
#include "fsshim.h"
#include "oldrt.h"
#include "shimreg.h"

// ---------------------------------------------------------------------------
// doubles in the OLD register order (hi, lo)
// ---------------------------------------------------------------------------
static TReal64 OldToDouble(TUint32 aHi, TUint32 aLo)
    {
    const TUint64 u = ((TUint64)aHi << 32) | (TUint64)aLo;
    TReal64 d;
    Mem::Copy(&d, &u, 8);
    return d;
    }

// Result for old code: r0 = hi, r1 = lo. A TUint64 result of an EABI function comes back as
// r0 = low 32 bits, r1 = high 32 bits, so build it from swapped halves.
static TUint64 DoubleToOld(TReal64 aD)
    {
    TUint64 u;
    Mem::Copy(&u, &aD, 8);
    const TUint32 hi = (TUint32)(u >> 32);
    const TUint32 lo = (TUint32)u;
    return ((TUint64)lo << 32) | (TUint64)hi;
    }

static TUint64 ShimFloatsidf(TInt aV)                                   // euser 1528
    {
    return DoubleToOld((TReal64)aV);
    }

static TUint64 ShimMuldf3(TUint32 aAh, TUint32 aAl, TUint32 aBh, TUint32 aBl)   // euser 1570
    {
    return DoubleToOld(OldToDouble(aAh, aAl) * OldToDouble(aBh, aBl));
    }

static TUint64 ShimDivdf3(TUint32 aAh, TUint32 aAl, TUint32 aBh, TUint32 aBl)   // euser 1512
    {
    const TReal64 b = OldToDouble(aBh, aBl);
    if (b == 0.0)
        {
        return DoubleToOld(0.0);        // old soft-float would trap; keep the game running
        }
    return DoubleToOld(OldToDouble(aAh, aAl) / b);
    }

static TInt ShimFixdfsi(TUint32 aHi, TUint32 aLo)                       // euser 1526
    {
    const TReal64 d = OldToDouble(aHi, aLo);
    if (d != d)
        {
        return 0;                       // NaN
        }
    if (d >= 2147483647.0)
        {
        return KMaxTInt;
        }
    if (d <= -2147483648.0)
        {
        return KMinTInt;
        }
    return (TInt)d;
    }

// euser 404: TReal Math::FRand(TInt64& aSeed). The seed is a TInt64 inside an old object, so it is
// only 4-byte aligned: work on a local copy.
static TUint64 ShimFRand(TUint32* aSeed)
    {
    TUint64 s = ((TUint64)aSeed[1] << 32) | (TUint64)aSeed[0];
    TInt64 seed = (TInt64)s;
    const TReal r = Math::FRand(seed);
    s = (TUint64)seed;
    aSeed[0] = (TUint32)s;
    aSeed[1] = (TUint32)(s >> 32);
    return DoubleToOld(r);
    }

// euser 869: TInt Math::Rand(TInt64& aSeed). Same seed handling as ShimFRand (euser 404): the seed
// is a TInt64 inside an old object, only 4-byte aligned, old word order low:high -- work on a local copy.
// Integer result, so no DoubleToOld.
static TInt ShimRand(TUint32* aSeed)
    {
    TUint64 s = ((TUint64)aSeed[1] << 32) | (TUint64)aSeed[0];
    TInt64 seed = (TInt64)s;
    const TInt r = Math::Rand(seed);
    s = (TUint64)seed;
    aSeed[0] = (TUint32)s;
    aSeed[1] = (TUint32)(s >> 32);
    return r;
    }

// euser 1636: __divdi3 -- verified at the call sites: dividend r0:r1 (low:high), divisor r2:r3,
// result r0:r1. Same as the EABI layout, so a plain TInt64 function matches.
static TInt64 ShimDivdi3(TInt64 aA, TInt64 aB)
    {
    return (aB == 0) ? (TInt64)0 : (TInt64)(aA / aB);
    }

// euser 1262: CBase::~CBase(this, flags). The old derived destructor passes its in-charge flags
// (3 in the log): bit 0 means "delete this" -- the old base destructor frees the object.
static void ShimCBaseDtor(TAny* aThis, TInt aFlags)
    {
    if ((aFlags & 1) && aThis)
        {
        User::Free(aThis);
        }
    }

// euser 326: TInt UserSvr::DllFileName(TInt aHandle, TDes16& aFileName): the .app's own path
static TInt ShimDllFileName(TInt /*aHandle*/, TDes16* aName)
    {
    const TDesC& p = FsShim_AppPath();
    aName->Copy(p.Left(aName->MaxLength()));
    return KErrNone;
    }

// estlib 21 / 22: double cos(double), double sin(double). Argument and result are doubles in the OLD word
// order (r0 = high word, r1 = low word), so they cannot be bound directly. Math::Cos / Math::Sin leave the
// result untouched on an invalid argument, so it starts at 0.
static TUint64 ShimCos(TUint32 aHi, TUint32 aLo)
    {
    TReal64 r = 0.0;
    const TReal64 x = OldToDouble(aHi, aLo);
    Math::Cos(r, x);
    return DoubleToOld(r);
    }

static TUint64 ShimSin(TUint32 aHi, TUint32 aLo)
    {
    TReal64 r = 0.0;
    const TReal64 x = OldToDouble(aHi, aLo);
    Math::Sin(r, x);
    return DoubleToOld(r);
    }

// euser 1569: __modsi3 (signed remainder helper of the old toolchain); zero divisor gives 0 like __divsi3.
static TInt ShimModsi3(TInt aNum, TInt aDen)
    {
    return aDen ? (aNum % aDen) : 0;
    }

// euser 1617: __umodsi3 (unsigned remainder helper of the old toolchain); a zero divisor gives 0, like __udivsi3.
static TUint ShimUmodsi3(TUint aNum, TUint aDen)
    {
    return aDen ? (aNum % aDen) : 0;
    }

static TUint64 ShimAdddf3(TUint32 aAh, TUint32 aAl, TUint32 aBh, TUint32 aBl)   // euser 1454
    {
    return DoubleToOld(OldToDouble(aAh, aAl) + OldToDouble(aBh, aBl));
    }

static TUint64 ShimNegdf2(TUint32 aHi, TUint32 aLo)                              // euser 1579
    {
    return DoubleToOld(-OldToDouble(aHi, aLo));
    }

// euser 1534 / 1541: __gtdf2 / __ltdf2 -- double comparison helpers of the old toolchain (libgcc fp-bit.c).
// Both doubles arrive in the OLD word order: a = r0 (high), r1 (low); b = r2 (high), r3 (low). The result
// is an int in r0 that the caller compares with 0 (checked at the four call sites in Snails.app):
//   __gtdf2: "a > b" is  result > 0      __ltdf2: "a < b" is  result < 0
// If either value is a NaN the answer must make BOTH tests false, so __gtdf2 answers -1 and __ltdf2 answers +1.
static TInt ShimGtdf2(TUint32 aAh, TUint32 aAl, TUint32 aBh, TUint32 aBl)
    {
    const TReal64 a = OldToDouble(aAh, aAl);
    const TReal64 b = OldToDouble(aBh, aBl);
    if (a != a || b != b)
        {
        return -1;
        }
    return (a > b) ? 1 : ((a < b) ? -1 : 0);
    }

static TInt ShimLtdf2(TUint32 aAh, TUint32 aAl, TUint32 aBh, TUint32 aBl)
    {
    const TReal64 a = OldToDouble(aAh, aAl);
    const TReal64 b = OldToDouble(aBh, aBl);
    if (a != a || b != b)
        {
        return 1;
        }
    return (a > b) ? 1 : ((a < b) ? -1 : 0);
    }
	
// ---------------------------------------------------------------------------
// estlib 133: void qsort(void* aBase, size_t aCount, size_t aSize, int (*aCmp)(const void*, const void*))
// aCmp is OLD code, so every comparison goes through TOldRt::CallArgs. If the comparator leaves, the sort
// stops (comparisons answer 0) and the leave is re-raised after the loop, so no partly swapped element
// is left behind. Up to 32 elements: insertion sort (stable, like a small BSD qsort). More: heap sort
// (no extra memory, no recursion).
// ---------------------------------------------------------------------------
static TInt SortCompare(TAny* aCmp, TInt& aLeave, const TUint8* aA, const TUint8* aB)
    {
    if (aLeave != KErrNone)
        {
        return 0;
        }
    TInt res = 0;
    const TInt reason = TOldRt::CallArgs(aCmp, (TInt)aA, (TInt)aB, 0, 0, res);
    if (reason != KErrNone)
        {
        aLeave = reason;
        return 0;
        }
    return res;
    }

static void SortSwap(TUint8* aA, TUint8* aB, TUint aSize)
    {
    while (aSize--)
        {
        const TUint8 t = *aA;
        *aA++ = *aB;
        *aB++ = t;
        }
    }

static void SortSift(TUint8* aBase, TUint aSize, TUint aRoot, TUint aCount, TAny* aCmp, TInt& aLeave)
    {
    for (;;)
        {
        TUint child = aRoot * 2 + 1;
        if (child >= aCount || aLeave != KErrNone)
            {
            return;
            }
        if (child + 1 < aCount &&
            SortCompare(aCmp, aLeave, aBase + child * aSize, aBase + (child + 1) * aSize) < 0)
            {
            child++;
            }
        if (SortCompare(aCmp, aLeave, aBase + aRoot * aSize, aBase + child * aSize) >= 0)
            {
            return;
            }
        SortSwap(aBase + aRoot * aSize, aBase + child * aSize, aSize);
        aRoot = child;
        }
    }

static void ShimQsort(TAny* aBase, TUint aCount, TUint aSize, TAny* aCmp)
    {
    if (!aBase || aCount < 2 || aSize == 0 || !aCmp)
        {
        return;
        }
    TUint8* base = (TUint8*)aBase;
    TInt leave = KErrNone;
    if (aCount <= 32)
        {
        for (TUint i = 1; i < aCount && leave == KErrNone; i++)
            {
            for (TUint j = i; j > 0; j--)
                {
                if (SortCompare(aCmp, leave, base + (j - 1) * aSize, base + j * aSize) <= 0)
                    {
                    break;
                    }
                SortSwap(base + (j - 1) * aSize, base + j * aSize, aSize);
                }
            }
        }
    else
        {
        for (TInt i = (TInt)(aCount / 2) - 1; i >= 0 && leave == KErrNone; i--)
            {
            SortSift(base, aSize, (TUint)i, aCount, aCmp, leave);
            }
        for (TUint end = aCount - 1; end > 0 && leave == KErrNone; end--)
            {
            SortSwap(base, base + end * aSize, aSize);
            SortSift(base, aSize, 0, end, aCmp, leave);
            }
        }
    if (leave != KErrNone)
        {
        TOldRt::Leave(leave);
        }
    }
	
// ---------------------------------------------------------------------------
// Single-precision helpers of the old toolchain (soft float). A float is a plain 32-bit IEEE value in ONE
// register (r0, and r1 for a second operand). Unlike doubles there is no word swap: the bits are only
// reinterpreted. Checked at the call sites in SDancer.app (4096.0f = 0x45800000 is loaded into r1 before
// __mulsf3; __nesf2 and __fixsfsi answer in r0).
// ---------------------------------------------------------------------------
static TReal32 BitsToFloat(TUint32 aBits)
    {
    TReal32 f;
    Mem::Copy(&f, &aBits, 4);
    return f;
    }

static TUint32 FloatToBits(TReal32 aF)
    {
    TUint32 u;
    Mem::Copy(&u, &aF, 4);
    return u;
    }

static TUint32 ShimMulsf3(TUint32 aA, TUint32 aB)                       // euser 1571
    {
    return FloatToBits(BitsToFloat(aA) * BitsToFloat(aB));
    }

// static TUint32 ShimDivsf3(TUint32 aA, TUint32 aB)                       // euser 1513
    // {
    // const TReal32 b = BitsToFloat(aB);
    // if (b == 0.0f)
        // {
        // return FloatToBits(0.0f);       // same policy as __divdf3: the old soft float would trap; keep the game running
        // }
    // return FloatToBits(BitsToFloat(aA) / b);
    // }

static TUint32 ShimDivsf3(TUint32 aA, TUint32 aB)                       // euser 1513
    {
    return FloatToBits(BitsToFloat(aA) / BitsToFloat(aB));
    }

static TInt ShimFixsfsi(TUint32 aBits)                                  // euser 1527: float -> int, toward zero
    {
    const TReal32 f = BitsToFloat(aBits);
    if (f != f)
        {
        return 0;                       // NaN
        }
    if (f >= 2147483648.0f)
        {
        return KMaxTInt;
        }
    if (f <= -2147483648.0f)
        {
        return KMinTInt;
        }
    return (TInt)f;
    }

// euser 1581: __nesf2 returns 0 only when a == b; the caller tests the result against 0.
// A NaN is "not equal" (1), like libgcc's fp-bit.c.
static TInt ShimNesf2(TUint32 aA, TUint32 aB)
    {
    const TReal32 a = BitsToFloat(aA);
    const TReal32 b = BitsToFloat(aB);
    if (a != a || b != b)
        {
        return 1;
        }
    return (a == b) ? 0 : ((a < b) ? -1 : 1);
    }

// euser 787: void TDes16::Num(TInt aVal). No v3 Num(int); widen to TInt64 and use v3 947
// Num(long long). The explicit cast keeps overload resolution away from 948/949.
static void ShimDesNumInt(TDes16* aDes, TInt aVal)
    {
    aDes->Num((TInt64)aVal);
    }

static const TShimRegEntry KRtShimEntries[] =
    {
    { "euser", 1528, (TUint32)&ShimFloatsidf, 0, "rtshim", NULL },
    { "euser", 1570, (TUint32)&ShimMuldf3, 0, "rtshim", NULL },
    { "euser", 1512, (TUint32)&ShimDivdf3, 0, "rtshim", NULL },
    { "euser", 1526, (TUint32)&ShimFixdfsi, 0, "rtshim", NULL },
    { "euser", 404,  (TUint32)&ShimFRand, 0, "rtshim", NULL },
	{ "euser", 787,  (TUint32)&ShimDesNumInt, 0, "rtshim", NULL },
	{ "euser", 869,  (TUint32)&ShimRand, 0, "rtshim", NULL },
    { "euser", 1636, (TUint32)&ShimDivdi3, 0, "rtshim", NULL },
    { "euser", 1262, (TUint32)&ShimCBaseDtor, 0, "rtshim", NULL },
    { "euser", 326,  (TUint32)&ShimDllFileName, 0, "rtshim", NULL },
    { "euser", 1569, (TUint32)&ShimModsi3, 0, "rtshim", NULL },
    { "euser", 1617, (TUint32)&ShimUmodsi3, 0, "rtshim", NULL },
    { "euser", 1454, (TUint32)&ShimAdddf3, 0, "rtshim", NULL },
    { "euser", 1579, (TUint32)&ShimNegdf2, 0, "rtshim", NULL },
	{ "euser", 1534, (TUint32)&ShimGtdf2, 0, "rtshim", NULL },
    { "euser", 1541, (TUint32)&ShimLtdf2, 0, "rtshim", NULL },
	{ "euser", 1513, (TUint32)&ShimDivsf3,  0, "rtshim", NULL },
    { "euser", 1527, (TUint32)&ShimFixsfsi, 0, "rtshim", NULL },
    { "euser", 1571, (TUint32)&ShimMulsf3,  0, "rtshim", NULL },
    { "euser", 1581, (TUint32)&ShimNesf2,   0, "rtshim", NULL },
    { "estlib", 21, (TUint32)&ShimCos, 0, "rtshim", NULL },   // cos(double)
    { "estlib", 22, (TUint32)&ShimSin, 0, "rtshim", NULL },    // sin(double)
	{ "estlib", 133, (TUint32)&ShimQsort, 0, "rtshim", NULL } // qsort(base, count, size, old comparator)
	};

TInt RtShim_Init()
    {
    return ShimReg_Add(KRtShimEntries, sizeof(KRtShimEntries) / sizeof(KRtShimEntries[0]));
    }

