#include "legacyimage.h"

const TUint32 KEpocSignature = 0x434F5045;   // 'EPOC'
const TUint32 KUidDll = 0x10000079;
const TUint32 KUidExe = 0x1000007A;
const TInt KMaxDllRefs = 64;
const TInt KMaxImportsPerDll = 8192;

CLegacyImage* CLegacyImage::NewL(const TDesC8& aData)
    {
    CLegacyImage* self = new (ELeave) CLegacyImage(aData);
    CleanupStack::PushL(self);
    self->ConstructL();
    CleanupStack::Pop(self);
    return self;
    }

CLegacyImage::~CLegacyImage()
    {
    iBlocks.Close();
    iOrdinals.Close();
    }

TBool CLegacyImage::InRange(TInt aOff, TInt aLen) const
    {
    return aOff >= 0 && aLen >= 0 && aOff <= iData.Length() && aLen <= iData.Length() - aOff;
    }

TUint32 CLegacyImage::Rd32(TInt aOff) const
    {
    TUint32 v = 0;
    if (InRange(aOff, 4))
        {
        Mem::Copy(&v, iData.Ptr() + aOff, 4);
        }
    return v;
    }

TUint16 CLegacyImage::Rd16(TInt aOff) const
    {
    TUint16 v = 0;
    if (InRange(aOff, 2))
        {
        Mem::Copy(&v, iData.Ptr() + aOff, 2);
        }
    return v;
    }

void CLegacyImage::ConstructL()
    {
    if (iData.Length() < 0x7C)
        {
        User::Leave(KErrCorrupt);
        }
    if (Rd32(0x10) != KEpocSignature)
        {
        User::Leave(KErrNotSupported);      // not an E32Image
        }
    iUid1 = Rd32(0x00);
    iUid2 = Rd32(0x04);
    iUid3 = Rd32(0x08);
    if (iUid1 != KUidDll && iUid1 != KUidExe)
        {
        User::Leave(KErrNotSupported);
        }
    if (Rd32(0x1C) != 0)
        {
        User::Leave(KErrNotSupported);      // compressed image
        }
    iToolsVer     = Rd32(0x20);
    iFlags        = Rd32(0x2C);
    iCodeSize     = Rd32(0x30);
    iDataSize     = Rd32(0x34);
    iHeapMin      = Rd32(0x38);
    iHeapMax      = Rd32(0x3C);
    iStack        = Rd32(0x40);
    iBssSize      = Rd32(0x44);
    iEntry        = Rd32(0x48);
    iCodeBase     = Rd32(0x4C);
    iDllRefCount  = Rd32(0x54);
    iExportOff    = Rd32(0x58);
    iExportCount  = Rd32(0x5C);
    iTextSize     = Rd32(0x60);
    iCodeOff      = Rd32(0x64);
    iImportOff    = Rd32(0x6C);
    iCodeRelocOff = Rd32(0x70);
    iDataRelocOff = Rd32(0x74);

    if (iDataSize != 0 || iBssSize != 0)
        {
        User::Leave(KErrNotSupported);      // writable static data: not handled yet
        }
    if (!InRange(iCodeOff, iCodeSize))
        {
        User::Leave(KErrCorrupt);
        }
    if (iDllRefCount > (TUint32)KMaxDllRefs)
        {
        User::Leave(KErrCorrupt);
        }

    // Import section
    if (iImportOff != 0)
        {
        if (!InRange(iImportOff, 4))
            {
            User::Leave(KErrCorrupt);
            }
        const TInt secSize = (TInt)Rd32(iImportOff);
        if (secSize < 4 || !InRange(iImportOff, secSize))
            {
            User::Leave(KErrCorrupt);
            }
        TInt pos = iImportOff + 4;
        for (TUint32 i = 0; i < iDllRefCount; i++)
            {
            if (!InRange(pos, 8))
                {
                User::Leave(KErrCorrupt);
                }
            TLegacyImportBlock b;
            b.iNameOff = (TInt)Rd32(pos);
            b.iCount   = (TInt)Rd32(pos + 4);
            b.iFirst   = iOrdinals.Count();
            if (b.iCount < 0 || b.iCount > KMaxImportsPerDll ||
                b.iNameOff < 0 || b.iNameOff >= secSize ||
                !InRange(pos + 8, b.iCount * 4))
                {
                User::Leave(KErrCorrupt);
                }
            for (TInt k = 0; k < b.iCount; k++)
                {
                User::LeaveIfError(iOrdinals.Append(Rd32(pos + 8 + 4 * k)));
                }
            User::LeaveIfError(iBlocks.Append(b));
            pos += 8 + 4 * b.iCount;
            }
        }
    }

TPtrC8 CLegacyImage::DllName(TInt aBlock) const
    {
    const TInt off = (TInt)iImportOff + iBlocks[aBlock].iNameOff;
    TInt max = iData.Length() - off;
    if (max > 80)
        {
        max = 80;
        }
    const TUint8* p = iData.Ptr() + off;
    TInt n = 0;
    while (n < max && p[n] != 0)
        {
        n++;
        }
    return TPtrC8(p, n);
    }

TUint32 CLegacyImage::ExportEntry(TInt aIndex) const
    {
    return Rd32((TInt)iExportOff + 4 * aIndex);
    }

TInt CLegacyImage::BuildCodeImage(TUint8* aDest, TUint32 aLoadAddr, TInt& aApplied, TInt& aBad) const
    {
    aApplied = 0;
    aBad = 0;
    Mem::Copy(aDest, iData.Ptr() + iCodeOff, iCodeSize);
    if (iCodeRelocOff == 0)
        {
        return KErrNone;
        }
    if (!InRange(iCodeRelocOff, 8))
        {
        return KErrCorrupt;
        }
    // Verified on a real file: iSize counts only the page blocks, NOT the
    // 8-byte section header {iSize, iNumberOfRelocs}.
    const TInt size = (TInt)Rd32(iCodeRelocOff);
    if (size < 0 || !InRange(iCodeRelocOff, 8 + size))
        {
        return KErrCorrupt;
        }
    TInt pos = iCodeRelocOff + 8;
    const TInt end = pos + size;
    while (pos + 8 <= end)
        {
        const TInt pageOff = (TInt)Rd32(pos);
        const TInt blk = (TInt)Rd32(pos + 4);
        if (blk < 8 || pos + blk > end)
            {
            return KErrCorrupt;
            }
        const TInt n = (blk - 8) / 2;
        for (TInt i = 0; i < n; i++)
            {
            const TUint16 e = Rd16(pos + 8 + 2 * i);
            const TInt type = e >> 12;
            if (type == 0)
                {
                continue;                       // padding
                }
            if (type == 2)
                {
                return KErrNotSupported;        // data relocation, no data section expected
                }
            const TInt target = pageOff + (e & 0x0FFF);
            if (target < 0 || target + 4 > (TInt)iCodeSize)
                {
                return KErrCorrupt;
                }
            TUint32 v;
            Mem::Copy(&v, aDest + target, 4);
            if (v >= iCodeBase && v < iCodeBase + iCodeSize)
                {
                v = v - iCodeBase + aLoadAddr;
                Mem::Copy(aDest + target, &v, 4);
                aApplied++;
                }
            else
                {
                aBad++;
                }
            }
        pos += blk;
        }
    return KErrNone;
    }
