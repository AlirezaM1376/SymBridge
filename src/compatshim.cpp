// Small, self-contained compatibility shims of the old game's euser / hal / plpvariant / sysagt imports.
//
// Everything here is a pure function of its arguments: no host globals (no AppUi, no old application
// object, no fake CCoeEnv), no calls back into old code, no per-thread state. That is the rule for
// this file; a shim that needs host state belongs next to that state (lhgui.cpp for now).
//
//  - descriptor constructors  TPtr8 / TPtr16 / TPtrC8 / TPtrC16 / TBufBase8 / TBufBase16 built in place on
//    old memory (same layout in v2 and v3, except the protected TBufBase ctors)
//  - TInt64 ctor, assignment, comparisons, operator-  (old TInt64 is a class of two words, v3 has a native type)
//  - TTime::HomeTime / UniversalTime / MicroSecondsFrom  (old by-value / hidden-pointer convention)
//  - TRect ctor / SetRect, Mem::Move, Mem::Compare, User::ReAlloc
//  - __divsi3, __udivsi3 compiler helpers
//  - HAL::Get, PlpVariant::GetMachineIdL, RSystemAgent ctor / Connect
//
// The table is added to the shim registry explicitly (CompatShim_Init), owner name "compatshim".
#include <e32std.h>
#include <e32base.h>
#include <hal.h>
#include "compatshim.h"
#include "shimreg.h"

// hal 1: HAL::Get(TAttribute, TInt&) -- same attribute numbering for the values games ask for
static TInt ShimHalGet(TInt aAttr, TInt* aValue)
    {
    return HAL::Get((HALData::TAttribute)aAttr, *aValue);
    }

// euser 1367: TPtr8(TUint8*, TInt aLength, TInt aMaxLength) on old-allocated memory.
// TPtr8 has the same layout in v2 and v3 (length, max length, pointer; no vptr).
static TAny* ShimTPtr8Ctor(TAny* aThis, TUint8* aBuf, TInt aLen, TInt aMax)
    {
    new (aThis) TPtr8(aBuf, aLen, aMax);
    return aThis;
    }

// euser 1366 / 1381: TPtr8(TUint8*, TInt aMax) / TPtr16(TUint16*, TInt aMax): same layout in v2 and v3.
static TAny* ShimTPtr8CtorMax(TAny* aThis, TUint8* aBuf, TInt aMax)
    {
    new (aThis) TPtr8(aBuf, aMax);
    return aThis;
    }

static TAny* ShimTPtr16CtorMax(TAny* aThis, TUint16* aBuf, TInt aMax)
    {
    new (aThis) TPtr16(aBuf, aMax);
    return aThis;
    }

// euser 1407: TPtrC16(const TUint16*) -- zero-terminated string. Layout (iLength, iPtr) is the same
// in v2 and v3. Left unshimmed this leaves the TPtrC uninitialised: the game's thread name was
// garbage and RThread::Create failed with KErrBadName (-28).
static TAny* ShimTPtrC16Ctor(TAny* aThis, const TUint16* aStr)
    {
    if (aStr)
        {
        new (aThis) TPtrC16(aStr);
        }
    else
        {
        new (aThis) TPtrC16();
        }
    return aThis;
    }

// The old TInt64 is a class of two words, possibly only 4-byte aligned in old objects, so every
// access goes through two 32-bit words. Verified by disassembly of the three call sites:
//   1379 TInt64(TInt):             r0 = this, r1 = value
//   1557 operator-(const TInt64&): r0 = hidden result pointer, r1 = this, r2 = rhs
//   1540 operator<(const TInt64&): r0 = this, r1 = rhs, result in r0
static TInt64 Ld64(const TAny* aP)
    {
    TUint32 w[2];
    Mem::Copy(w, aP, 8);
    return (TInt64)(((TUint64)w[1] << 32) | (TUint64)w[0]);
    }

static void St64(TAny* aP, TInt64 aV)
    {
    const TUint64 u = (TUint64)aV;
    TUint32 w[2];
    w[0] = (TUint32)u;
    w[1] = (TUint32)(u >> 32);
    Mem::Copy(aP, w, 8);
    }

static TAny* ShimTInt64Ctor(TAny* aThis, TInt aVal)
    {
    St64(aThis, (TInt64)aVal);
    return aThis;
    }

static TAny* ShimTInt64Sub(TAny* aRet, const TAny* aThis, const TAny* aRhs)
    {
    St64(aRet, Ld64(aThis) - Ld64(aRhs));
    return aRet;
    }

static TInt ShimTInt64Less(const TAny* aThis, const TAny* aRhs)
    {
    return Ld64(aThis) < Ld64(aRhs);
    }

// euser 1496: TInt64& TInt64::operator=(TInt aVal)
static TAny* ShimTInt64AssignInt(TAny* aThis, TInt aVal)
    {
    St64(aThis, (TInt64)aVal);
    return aThis;
    }

// euser 1530 / 1533: TInt TInt64::operator>=(const TInt64&) const  /  operator>(const TInt64&) const
static TInt ShimTInt64GreaterEq(const TAny* aThis, const TAny* aRhs)
    {
    return Ld64(aThis) >= Ld64(aRhs);
    }

static TInt ShimTInt64Greater(const TAny* aThis, const TAny* aRhs)
    {
    return Ld64(aThis) > Ld64(aRhs);
    }

// euser 1283: TBufBase16(TInt aMaxLength). The ctor is protected in v3, so build a trivial
// subclass (same size and layout) with placement new on the old object.
class TBufBase16Access : public TBufBase16
    {
public:
    TBufBase16Access(TInt aMax) : TBufBase16(aMax) {}
    };

static TAny* ShimTBufBase16Ctor(TAny* aThis, TInt aMax)
    {
    new (aThis) TBufBase16Access(aMax);
    return aThis;
    }

// euser 531: TTime::HomeTime() sets *this. The destination may be only 4-byte aligned,
// so write it as two 32-bit words (v3 code could use an 8-byte store).
static void ShimHomeTime(TAny* aThis)
    {
    TTime t;
    t.HomeTime();
    const TInt64 v = t.Int64();
    TUint32* p = (TUint32*)aThis;
    p[0] = I64LOW(v);
    p[1] = I64HIGH(v);
    }

// euser 695: TTimeIntervalMicroSeconds TTime::MicroSecondsFrom(TTime) const.
// Verified in the binary: r0 = hidden result pointer, r1 = this, the by-value TTime in
// r2:r3 (the old ABI packs it right after this; EABI would align it differently).
static void ShimMicroSecondsFrom(TAny* aRet, TAny* aThis, TUint32 aLo, TUint32 aHi)
    {
    const TUint32* t = (const TUint32*)aThis;
    const TInt64 a = ((TInt64)t[1] << 32) | (TInt64)t[0];     // TInt64 is a native type in this SDK
    const TInt64 b = ((TInt64)aHi << 32) | (TInt64)aLo;
    const TInt64 r = a - b;
    TUint32* o = (TUint32*)aRet;
    o[0] = I64LOW(r);
    o[1] = I64HIGH(r);
    }

// euser 1514: __divsi3 (compiler helper of the old toolchain)
static TInt ShimDivsi3(TInt aNum, TInt aDen)
    {
    return aDen ? (aNum / aDen) : 0;
    }

// euser 1616: __udivsi3. A game divided (1600000, 16000) in its engine setup: returning 0 from the trace stub
// would have fed a wrong value into it.
static TUint ShimUdivsi3(TUint aNum, TUint aDen)
    {
    return aDen ? (aNum / aDen) : 0;
    }

// euser 1166: TTime::UniversalTime() sets *this; the destination may be only 4-byte aligned, so write two
// 32-bit words (same reason as HomeTime above).
static void ShimUniversalTime(TAny* aThis)
    {
    TTime t;
    t.UniversalTime();
    const TInt64 v = t.Int64();
    TUint32* p = (TUint32*)aThis;
    p[0] = I64LOW(v);
    p[1] = I64HIGH(v);
    }

// euser 1382: TPtr16(TUint16* aBuf, TInt aLength, TInt aMaxLength)
static TAny* ShimTPtr16Ctor(TAny* aThis, TUint16* aBuf, TInt aLen, TInt aMax)
    {
    new (aThis) TPtr16(aBuf, aLen, aMax);
    return aThis;
    }

// euser 1431 / 1432: TBufBase8(TInt aMax) and TBufBase8(TInt aLength, TInt aMax). Protected in v3, like the
// 16-bit one: build a trivial subclass in place.
class TBufBase8Access : public TBufBase8
    {
public:
    TBufBase8Access(TInt aMax) : TBufBase8(aMax) {}
    TBufBase8Access(TInt aLen, TInt aMax) : TBufBase8(aLen, aMax) {}
    };

static TAny* ShimTBufBase8Ctor1(TAny* aThis, TInt aMax)
    {
    new (aThis) TBufBase8Access(aMax);
    return aThis;
    }

static TAny* ShimTBufBase8Ctor2(TAny* aThis, TInt aLen, TInt aMax)
    {
    new (aThis) TBufBase8Access(aLen, aMax);
    return aThis;
	}

// euser 75: TDes8& TDes8::AppendNum(TUint aVal, TRadix aRadix). v3 only has the 64 bit variant (ordinal 779).
// The descriptor is a real v3 TBufBase8 (built in place by the constructor shim above), so its own method runs.
static TDes8* ShimAppendNum(TDes8* aThis, TUint aVal, TInt aRadix)
    {
    aThis->AppendNum((TUint64)aVal, (TRadix)aRadix);
    return aThis;
    }

// euser 709: Mem::Move(void* aTrg, const void* aSrc, TInt aLength) has no v3 export. Copy that is safe for
// overlapping areas, returns aTrg.
static TAny* ShimMemMove(TUint8* aTrg, const TUint8* aSrc, TInt aLen)
    {
    if (aLen > 0 && aTrg != aSrc)
        {
        if (aTrg < aSrc)
            {
            for (TInt i = 0; i < aLen; i++)
                {
                aTrg[i] = aSrc[i];
                }
            }
        else
            {
            for (TInt i = aLen - 1; i >= 0; i--)
                {
                aTrg[i] = aSrc[i];
                }
            }
        }
    return aTrg;
    }

// euser 1369: TRect::TRect()
static TAny* ShimTRectCtor(TAny* aThis)
    {
    new (aThis) TRect();
    return aThis;
    }

// euser 1021: void TRect::SetRect(const TPoint&, const TSize&)
static void ShimTRectSetRect(TRect* aThis, const TPoint* aPoint, const TSize* aSize)
    {
    aThis->SetRect(*aPoint, *aSize);
    }

// euser 1384 / 1386 / 1406: TPtrC8() / TPtrC8(const TUint8*, TInt) / TPtrC16(), built in place
static TAny* ShimTPtrC8Ctor0(TAny* aThis)
    {
    new (aThis) TPtrC8();
    return aThis;
    }

static TAny* ShimTPtrC8Ctor2(TAny* aThis, const TUint8* aPtr, TInt aLen)
    {
    new (aThis) TPtrC8(aPtr, aLen);
    return aThis;
    }

static TAny* ShimTPtrC16Ctor0(TAny* aThis)
    {
    new (aThis) TPtrC16();
    return aThis;
    }

// euser 1281: TBufBase16(const TUint16* aString, TInt aMaxLength): the descriptor is a TBuf<N> that
// is built from a string (the game builds TBuf<256> from a literal). Copy panics when the string
// is longer than aMaxLength, as on a real device.
static TAny* ShimTBufBase16CtorStr(TAny* aThis, const TUint16* aStr, TInt aMax)
    {
    TBufBase16Access* b = new (aThis) TBufBase16Access(aMax);
    if (aStr)
        {
        b->Copy(aStr);
        }
    return aThis;
    }

// euser 875: TAny* User::ReAlloc(TAny*, TInt). v3 has a third parameter (mode), the map row is "name only".
static TAny* ShimReAlloc(TAny* aPtr, TInt aSize)
    {
    return User::ReAlloc(aPtr, aSize, 0);
    }

// euser 202: TInt Mem::Compare(const TUint8*, TInt, const TUint8*, TInt) -- no v3 export. Returns
// the difference of the first differing bytes, else the difference of the lengths.
static TInt ShimMemCompare(const TUint8* aL, TInt aLL, const TUint8* aR, TInt aRL)
    {
    const TInt n = (aLL < aRL) ? aLL : aRL;
    for (TInt i = 0; i < n; i++)
        {
        if (aL[i] != aR[i])
            {
            return (TInt)aL[i] - (TInt)aR[i];
            }
        }
    return aLL - aRL;
    }

// plpvariant 6: PlpVariant::GetMachineIdL(TBuf<128>&) has no v3 equivalent. On a real phone it is
// the IMEI. Some games read exactly 15 characters, fewer would panic in operator[], so a fixed
// 15-digit value is returned. It is a well-known public example number, not a real device.
// A profile key (machine_id) could replace the fixed value for a game that needs another one.
static void ShimGetMachineIdL(TDes16* aBuf)
    {
    aBuf->Copy(_L("540806859904945"));
    }

// sysagt 18: RSystemAgent::RSystemAgent()
static TAny* ShimSysAgentCtor(TAny* aThis)
    {
    *(TUint32*)aThis = 0;                       // RSessionBase: one handle word, 0 = not open
    return aThis;
    }

// sysagt 20: TInt RSystemAgentBase::Connect()
static TInt ShimSysAgentConnect(TAny* /*aThis*/)
    {
    return KErrNone;
    }

static const TShimRegEntry KCompatShimEntries[] =
    {
	{ "euser", 75,   (TUint32)&ShimAppendNum,      0, "compatshim", NULL },   // TDes8::AppendNum(unsigned int, TRadix)
    { "euser",  202, (TUint32)&ShimMemCompare, 0, "compatshim", NULL },   // Mem::Compare(const TUint8*, TInt, const TUint8*, TInt)
    { "euser",  531, (TUint32)&ShimHomeTime, 0, "compatshim", NULL },   // TTime::HomeTime()
    { "euser",  695, (TUint32)&ShimMicroSecondsFrom, 0, "compatshim", NULL },   // TTime::MicroSecondsFrom(TTime) const
    { "euser",  709, (TUint32)&ShimMemMove, 0, "compatshim", NULL },   // Mem::Move(void*, const void*, TInt)
    { "euser",  875, (TUint32)&ShimReAlloc, 0, "compatshim", NULL },   // User::ReAlloc(void*, TInt)
    { "euser", 1021, (TUint32)&ShimTRectSetRect, 0, "compatshim", NULL },   // TRect::SetRect(const TPoint&, const TSize&)
    { "euser", 1166, (TUint32)&ShimUniversalTime, 0, "compatshim", NULL },   // TTime::UniversalTime()
    { "euser", 1281, (TUint32)&ShimTBufBase16CtorStr, 0, "compatshim", NULL },   // TBufBase16::TBufBase16(const TUint16*, TInt)
    { "euser", 1283, (TUint32)&ShimTBufBase16Ctor, 0, "compatshim", NULL },   // TBufBase16::TBufBase16(TInt)
    { "euser", 1366, (TUint32)&ShimTPtr8CtorMax, 0, "compatshim", NULL },   // TPtr8::TPtr8(TUint8*, TInt)
    { "euser", 1367, (TUint32)&ShimTPtr8Ctor, 0, "compatshim", NULL },   // TPtr8::TPtr8(TUint8*, TInt, TInt)
    { "euser", 1369, (TUint32)&ShimTRectCtor, 0, "compatshim", NULL },   // TRect::TRect()
    { "euser", 1379, (TUint32)&ShimTInt64Ctor, 0, "compatshim", NULL },   // TInt64::TInt64(int)
    { "euser", 1381, (TUint32)&ShimTPtr16CtorMax, 0, "compatshim", NULL },   // TPtr16::TPtr16(TUint16*, TInt)
    { "euser", 1382, (TUint32)&ShimTPtr16Ctor, 0, "compatshim", NULL },   // TPtr16::TPtr16(TUint16*, TInt, TInt)
    { "euser", 1384, (TUint32)&ShimTPtrC8Ctor0, 0, "compatshim", NULL },   // TPtrC8::TPtrC8()
    { "euser", 1386, (TUint32)&ShimTPtrC8Ctor2, 0, "compatshim", NULL },   // TPtrC8::TPtrC8(const TUint8*, TInt)
    { "euser", 1406, (TUint32)&ShimTPtrC16Ctor0, 0, "compatshim", NULL },   // TPtrC16::TPtrC16()
    { "euser", 1407, (TUint32)&ShimTPtrC16Ctor, 0, "compatshim", NULL },   // TPtrC16::TPtrC16(const TUint16*)
    { "euser", 1431, (TUint32)&ShimTBufBase8Ctor1, 0, "compatshim", NULL },   // TBufBase8::TBufBase8(TInt)
    { "euser", 1432, (TUint32)&ShimTBufBase8Ctor2, 0, "compatshim", NULL },   // TBufBase8::TBufBase8(TInt, TInt)
    { "euser", 1496, (TUint32)&ShimTInt64AssignInt, 0, "compatshim", NULL },   // TInt64::operator=(int)
    { "euser", 1514, (TUint32)&ShimDivsi3, 0, "compatshim", NULL },   // __divsi3
    { "euser", 1530, (TUint32)&ShimTInt64GreaterEq, 0, "compatshim", NULL },   // TInt64::operator>=(const TInt64&) const
    { "euser", 1533, (TUint32)&ShimTInt64Greater, 0, "compatshim", NULL },   // TInt64::operator>(const TInt64&) const
    { "euser", 1540, (TUint32)&ShimTInt64Less, 0, "compatshim", NULL },   // TInt64::operator<(const TInt64&) const
    { "euser", 1557, (TUint32)&ShimTInt64Sub, 0, "compatshim", NULL },   // TInt64::operator-(const TInt64&) const
    { "euser", 1616, (TUint32)&ShimUdivsi3, 0, "compatshim", NULL },   // __udivsi3
    { "hal",    1, (TUint32)&ShimHalGet, 0, "compatshim", NULL },   // HAL::Get(TAttribute, TInt&)
    { "plpvariant",    6, (TUint32)&ShimGetMachineIdL, 0, "compatshim", NULL },   // PlpVariant::GetMachineIdL(TBuf<128>&)
    { "sysagt",   18, (TUint32)&ShimSysAgentCtor, 0, "compatshim", NULL },   // RSystemAgent::RSystemAgent()
    { "sysagt",   20, (TUint32)&ShimSysAgentConnect, 0, "compatshim", NULL }   // RSystemAgentBase::Connect()
    };

TInt CompatShim_Init()
    {
    return ShimReg_Add(KCompatShimEntries, sizeof(KCompatShimEntries) / sizeof(KCompatShimEntries[0]));
    }
