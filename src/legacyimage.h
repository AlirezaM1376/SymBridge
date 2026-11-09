// Parser for the pre-Symbian-9 (ER5 style) E32Image format, as used by
// S60 1st/2nd edition .app/.dll/.exe files.
// Layout verified offline against a real S60v2 .app file:
//   - header fields at fixed offsets (see ConstructL)
//   - "code region" = text section + import address table (IAT) + export dir
//   - import section = blocks of {dllNameOffset, count, ordinals[count]}
//   - the IAT words in the code region hold the same ordinals, in the same order
//   - relocation section = pages of {pageOffset, blockSize, uint16 entries},
//     entry = (type << 12) | offsetInPage; offsets are relative to code start
#ifndef LEGACYIMAGE_H
#define LEGACYIMAGE_H

#include <e32base.h>
#include <e32std.h>

class TLegacyImportBlock
    {
public:
    TInt iNameOff;   // relative to import section start
    TInt iFirst;     // index of first ordinal in CLegacyImage::iOrdinals
    TInt iCount;
    };

class CLegacyImage : public CBase
    {
public:
    // aData must stay alive as long as this object.
    static CLegacyImage* NewL(const TDesC8& aData);
    ~CLegacyImage();

    TPtrC8 DllName(TInt aBlock) const;
    TUint32 ExportEntry(TInt aIndex) const;

    // Copies the code region to aDest (iCodeSize bytes) and relocates it so
    // that it is valid when located at aLoadAddr.
    // aBad counts relocation slots whose value was outside the image.
    TInt BuildCodeImage(TUint8* aDest, TUint32 aLoadAddr, TInt& aApplied, TInt& aBad) const;

public:
    TUint32 iUid1, iUid2, iUid3;
    TUint32 iFlags, iToolsVer;
    TUint32 iCodeSize, iDataSize, iBssSize;
    TUint32 iHeapMin, iHeapMax, iStack;
    TUint32 iEntry, iCodeBase, iDllRefCount;
    TUint32 iExportOff, iExportCount;
    TUint32 iTextSize, iCodeOff, iImportOff;
    TUint32 iCodeRelocOff, iDataRelocOff;
    RArray<TLegacyImportBlock> iBlocks;
    RArray<TUint32> iOrdinals;

private:
    CLegacyImage(const TDesC8& aData) : iData(aData) {}
    void ConstructL();
    TUint32 Rd32(TInt aOff) const;
    TUint16 Rd16(TInt aOff) const;
    TBool InRange(TInt aOff, TInt aLen) const;

    const TDesC8& iData;
    };

#endif
