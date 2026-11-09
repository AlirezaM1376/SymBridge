#include "swscale.h"

// RGB565 -> 0000 0ggg ggg0 0000 rrrr r000 000b bbbb (green moved 16 bits up). With the gaps between
// the channels, a channel can be multiplied by a weight of up to 32 without spilling into its neighbour.
static const TUint32 KSpreadMask = 0x07E0F81F;

static inline TUint32 Spread(TUint32 aPix)
    {
    return (aPix | (aPix << 16)) & KSpreadMask;
    }

// (a * (32 - w) + b * w) / 32 on all three channels at once; w is 0..32.
static inline TUint32 Lerp(TUint32 aA, TUint32 aB, TUint32 aW)
    {
    return ((aA * (32 - aW) + aB * aW) >> 5) & KSpreadMask;
    }

static void DeleteArray16(TAny* aPtr) { delete[] (TUint16*)aPtr; }
static void DeleteArray8(TAny* aPtr) { delete[] (TUint8*)aPtr; }

CBilinearScaler* CBilinearScaler::NewL(const TSize& aSrc, const TSize& aDst)
    {
    if (aSrc.iWidth < 1 || aSrc.iHeight < 1 || aDst.iWidth < 1 || aDst.iHeight < 1 ||
        aSrc.iWidth > 2000 || aSrc.iHeight > 2000 || aDst.iWidth > 2000 || aDst.iHeight > 2000)
        {
        User::Leave(KErrArgument);
        }
    CBilinearScaler* self = new (ELeave) CBilinearScaler(aSrc, aDst);
    CleanupStack::PushL(self);
    self->ConstructL();
    CleanupStack::Pop(self);
    return self;
    }

void CBilinearScaler::ConstructL()
    {
    iXT = new (ELeave) TUint32[iDst.iWidth];
    iY0 = new (ELeave) TUint16[iDst.iHeight];
    iY1 = new (ELeave) TUint16[iDst.iHeight];
    iWy = new (ELeave) TUint8[iDst.iHeight];
    iRows = new (ELeave) TUint32[iDst.iWidth * 2];
    iSpread = new (ELeave) TUint32[iSrc.iWidth + 1];
    TUint16* x0 = new (ELeave) TUint16[iDst.iWidth];
    CleanupStack::PushL(TCleanupItem(DeleteArray16, x0));
    TUint16* x1 = new (ELeave) TUint16[iDst.iWidth];
    CleanupStack::PushL(TCleanupItem(DeleteArray16, x1));
    TUint8* wx = new (ELeave) TUint8[iDst.iWidth];
    CleanupStack::PushL(TCleanupItem(DeleteArray8, wx));
    BuildAxis(iSrc.iWidth, iDst.iWidth, x0, x1, wx);
    for (TInt i = 0; i < iDst.iWidth; i++)
        {
        iXT[i] = Pack(x0[i], wx[i]);
        }
    CleanupStack::PopAndDestroy(3);
    BuildAxis(iSrc.iHeight, iDst.iHeight, iY0, iY1, iWy);
    }

CBilinearScaler::~CBilinearScaler()
    {
    delete[] iXT;
    delete[] iY0;
    delete[] iY1;
    delete[] iWy;
    delete[] iRows;
    delete[] iSpread;
    }

// Maps every output index to two source indices and a weight. Pixel centres are aligned
// (output pixel i samples the source at (i + 0.5) * src/dst - 0.5), so the picture is not shifted
// and the edges are not darker or lighter. Positions are 16.16 fixed point.
void CBilinearScaler::BuildAxis(TInt aSrcLen, TInt aDstLen, TUint16* aIdx0, TUint16* aIdx1, TUint8* aWeight)
    {
    const TInt step = (aSrcLen << 16) / aDstLen;
    const TInt maxPos = (aSrcLen - 1) << 16;
    for (TInt i = 0; i < aDstLen; i++)
        {
        TInt pos = i * step + (step >> 1) - 0x8000;
        if (pos < 0)
            {
            pos = 0;
            }
        if (pos > maxPos)
            {
            pos = maxPos;
            }
        const TInt i0 = pos >> 16;
        aIdx0[i] = (TUint16)i0;
        aIdx1[i] = (TUint16)((i0 + 1 < aSrcLen) ? i0 + 1 : i0);
        aWeight[i] = (TUint8)((pos >> 11) & 31);
        }
    }

// One source row scaled to the output width, left in the spread format (not packed back to RGB565).
// The row is spread once into iSpread; the right neighbour of the last column is a copy of it, so
// "left + 1" is always readable (its weight is 0 there).
void CBilinearScaler::HorizontalRow(const TUint16* aSrcRow, TUint32* aOut) const
    {
    const TInt sw = iSrc.iWidth;
    TUint32* spr = iSpread;
    for (TInt i = 0; i < sw; i++)
        {
        spr[i] = Spread(aSrcRow[i]);
        }
    spr[sw] = spr[sw - 1];
    const TInt dw = iDst.iWidth;
    const TUint32* xt = iXT;
    for (TInt x = 0; x < dw; x++)
        {
        const TUint32 e = xt[x];
        const TUint32* p = spr + (e & 0xFFFF);
        aOut[x] = Lerp(p[0], p[1], e >> 16);
        }
    }

void CBilinearScaler::Scale(const TUint8* aSrc, TInt aSrcStride, TUint8* aDst, TInt aDstStride)
    {
    const TInt dw = iDst.iWidth;
    const TInt dh = iDst.iHeight;
    TUint32* slot[2] = { iRows, iRows + dw };
    TInt key[2] = { -1, -1 };                       // source row held by each slot
    for (TInt y = 0; y < dh; y++)
        {
        const TInt y0 = iY0[y];
        const TUint32 wy = iWy[y];
        const TInt y1 = wy ? iY1[y] : y0;               // weight 0: the bottom row is not needed at all
        // Both rows are needed in two different slots. Output rows come in order, so the bottom row of
        // one output row is usually the top row of the next and is already there.
        TInt sa = (key[0] == y0) ? 0 : ((key[1] == y0) ? 1 : -1);
        TInt sb = (y1 == y0) ? sa : ((key[0] == y1) ? 0 : ((key[1] == y1) ? 1 : -1));
        if (sa < 0)
            {
            sa = (sb == 0) ? 1 : 0;
            HorizontalRow((const TUint16*)(aSrc + y0 * aSrcStride), slot[sa]);
            key[sa] = y0;
            if (y1 == y0)
                {
                sb = sa;
                }
            }
        if (sb < 0)
            {
            sb = 1 - sa;
            HorizontalRow((const TUint16*)(aSrc + y1 * aSrcStride), slot[sb]);
            key[sb] = y1;
            }
        const TUint32* a = slot[sa];
        const TUint32* b = slot[sb];
        TUint16* out = (TUint16*)(aDst + y * aDstStride);
        if (wy == 0 || sa == sb)
            {
            for (TInt x = 0; x < dw; x++)
                {
                const TUint32 o = a[x];
                out[x] = (TUint16)(o | (o >> 16));
                }
            }
        else
            {
            // Two pixels per round: half the loop overhead, and the second pair of loads overlaps
            // the multiply of the first.
            const TUint32 iw = 32 - wy;
            TInt x = 0;
            for (; x + 2 <= dw; x += 2)
                {
                const TUint32 a0 = a[x];
                const TUint32 b0 = b[x];
                const TUint32 a1 = a[x + 1];
                const TUint32 b1 = b[x + 1];
                const TUint32 o0 = ((a0 * iw + b0 * wy) >> 5) & KSpreadMask;
                const TUint32 o1 = ((a1 * iw + b1 * wy) >> 5) & KSpreadMask;
                out[x] = (TUint16)(o0 | (o0 >> 16));
                out[x + 1] = (TUint16)(o1 | (o1 >> 16));
                }
            if (x < dw)
                {
                const TUint32 o = Lerp(a[x], b[x], wy);
                out[x] = (TUint16)(o | (o >> 16));
                }
            }
        }
    }
