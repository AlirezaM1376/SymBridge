// Software bilinear scaler for RGB565 (EColor64K) pictures.
// Used by the bilinear scale modes of configs.cfg (scale_mode 3 and 5).
//
// The sample positions and weights are computed once per size pair (NewL). Scale() then works in two
// passes that share work: the horizontal pass is done once per source row that is needed (and kept for
// the next output row, which usually needs the same one), the vertical pass blends two such rows.
// Each lerp works on a whole RGB565 pixel at once (channels spread over a 32-bit word), so it costs
// one multiply-accumulate pair instead of one per channel. Every source row is spread once (not once
// per output pixel), and the column table is one 32-bit word per output column (index + weight).
#ifndef SWSCALE_H
#define SWSCALE_H

#include <e32base.h>

class CBilinearScaler : public CBase
    {
public:
    static CBilinearScaler* NewL(const TSize& aSrc, const TSize& aDst);
    ~CBilinearScaler();

    const TSize& SrcSize() const { return iSrc; }
    const TSize& DstSize() const { return iDst; }

    // aSrc / aDst point at 16-bit aligned RGB565 rows; the strides are in bytes.
    // aSrc must hold SrcSize() pixels and aDst must have room for DstSize() pixels.
    void Scale(const TUint8* aSrc, TInt aSrcStride, TUint8* aDst, TInt aDstStride);

private:
    CBilinearScaler(const TSize& aSrc, const TSize& aDst) : iSrc(aSrc), iDst(aDst) {}
    void ConstructL();
    static void BuildAxis(TInt aSrcLen, TInt aDstLen, TUint16* aIdx0, TUint16* aIdx1, TUint8* aWeight);
    static TUint32 Pack(TUint16 aIdx, TUint8 aWeight) { return (TUint32)aIdx | ((TUint32)aWeight << 16); }
    void HorizontalRow(const TUint16* aSrcRow, TUint32* aOut) const;

private:
    TSize iSrc;
    TSize iDst;
    TUint32* iXT;           // per output column: left source column | weight of the right column << 16
                            // (right column = left + 1; at the right edge the weight is 0)
    TUint16* iY0;           // per output row:    top source row
    TUint16* iY1;           //                    bottom source row (clamped)
    TUint8*  iWy;           //                    weight of the bottom row, 0..31 (of 32)
    TUint32* iRows;         // two horizontally scaled rows (spread RGB565), DstSize().iWidth each
    TUint32* iSpread;       // the current source row in spread format, SrcSize().iWidth + 1 words
    };

#endif
