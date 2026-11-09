#include <e32base.h>
#include <f32file.h>
#include "fsshim.h"
#include "crashwatch.h"
#include "shimreg.h"
#include "importmap.h"

static THostLog* gLog = NULL;
static TFileName gAppFile;
static TFileName gAppRoot;                           // drive and folder of the game, ends with '\'
static TInt gDiagLogs[3];

// Maps old absolute install paths into the game folder. Returns EFalse when the name is left alone.
static TBool RemapOldPath(const TDesC& aName, TFileName& aOut)
    {
    TInt start = 0;
    for (TInt i = aName.Length() - 3; i >= 1; i--)    // i = index of a drive letter
        {
        if (aName[i + 1] == ':' && aName[i + 2] == '\\' && aName[i - 1] == '\\')
            {
            start = i;
            break;
            }
        }
    const TPtrC abs(aName.Mid(start));
    if (abs.Length() < 4 || abs[1] != ':' || abs[2] != '\\')
        {
        return EFalse;
        }
    const TPtrC p(abs.Mid(2));                        // starts with '\'
    _LIT(KApps, "\\system\\apps\\");
    TPtrC rest(p.Mid(1));
    if (p.Length() > KApps().Length() && p.Left(KApps().Length()).CompareF(KApps) == 0)
        {
        TPtrC afterApps(p.Mid(KApps().Length()));
        const TInt slash = afterApps.Locate('\\');
        if (slash < 0)
            {
            return EFalse;
            }
        rest.Set(afterApps.Mid(slash + 1));
        }
    else if (start == 0)
        {
        return EFalse;                                // plain absolute path: not ours to change
        }
    if (gAppRoot.Length() + rest.Length() > aOut.MaxLength())
        {
        return EFalse;
        }
    aOut.Copy(gAppRoot);
    aOut.Append(rest);
    return ETrue;
    }

// Same mapping for other host modules (gfxfake loads the game's .mbm through it).
TBool FsShim_RemapOldPath(const TDesC& aName, TFileName& aOut)
    {
	if (aName.Length() > 0 && aName[0] == '\\' && aName.Length() + 2 <= KMaxFileName)
	{
		TFileName withDrive;                  // "\System\Apps\Game\x.mbm" has no drive: RemapOldPath needs one
		withDrive.Copy(_L("C:"));
		withDrive.Append(aName);
		return RemapOldPath(withDrive, aOut);
	}
    return RemapOldPath(aName, aOut);
    }
	
static TInt gRemapLogs = 0;
static void LogRemap(const TDesC& aFrom, const TDesC& aTo)
    {
    if (gLog && gRemapLogs < 6)
        {
        gRemapLogs++;
        TBuf8<140> a, b;
        a.Copy(aFrom.Left(a.MaxLength()));
        b.Copy(aTo.Left(b.MaxLength()));
        gLog->Logf(_L8("fs: path remapped '%S' -> '%S'"), &a, &b);
        }
    }

// Old (v2) TParse layout, verified from the v2 f32file.h:
//   TParseBase: vptr (4) | iWild (4) | SField iField[4] (16)  = 24 bytes
//   TParse    : + TFileName iNameBuf (8 + 512)               = 544 bytes
const TInt KOldNameOff   = 24;
const TInt KOldParseSize = 544;
typedef char TCheckFileName[(sizeof(TFileName) == 520) ? 1 : -1];
typedef char TCheckPtrC[(sizeof(TPtrC) == 8) ? 1 : -1];

// ---------------------------------------------------------------------------
// TParse objects created by our ctor shim (old code keeps them on its stack).
// ---------------------------------------------------------------------------
const TInt KMaxParse = 128;                 // power of two
static TUint32 gParse[KMaxParse];
static TInt gParseNext = 0;

static void RegisterParse(TUint32 aP)
    {
    for (TInt i = 0; i < KMaxParse; i++)
        {
        if (gParse[i] == aP)
            {
            return;
            }
        }
    gParse[gParseNext & (KMaxParse - 1)] = aP;
    gParseNext++;
    }

static TBool IsParse(TUint32 aP)
    {
    if (aP == 0)
        {
        return EFalse;
        }
    for (TInt i = 0; i < KMaxParse; i++)
        {
        if (gParse[i] == aP)
            {
            return ETrue;
            }
        }
    return EFalse;
    }

// Fake GCC 2.95 vtable for TParse (2 header words, then slots). Old code never calls
// its virtuals (NameBuf/NameBufC are protected), but an unexpected call must be visible.
static TUint32 gVtParse[8];
static TInt gVtLogs = 0;

static TInt ParseVirtualStub()
    {
    if (gLog && gVtLogs < 5)
        {
        gVtLogs++;
        gLog->Text(_L8("fs: UNEXPECTED virtual call on facade TParse (returns 0)"));
        }
    return 0;
    }

// ---------------------------------------------------------------------------
// TParse facade. State lives in the old object's name buffer only; every query rebuilds a
// real v3 TParse from it (stateless, so the facade needs no v3 vtable on the old object).
// ---------------------------------------------------------------------------
static TInt gParseLogs = 0;

static TAny* ShimParseCtor(TUint8* aThis)              // efsrv 230: TParse::TParse()
    {
    Mem::FillZ(aThis, KOldParseSize);
    *(TUint32*)aThis = (TUint32)gVtParse;
    new (aThis + KOldNameOff) TFileName;               // length 0, max length 256
    RegisterParse((TUint32)aThis);
    return aThis;
    }

// efsrv 183: TInt TParse::Set(const TDesC& aName, const TDesC* aRelated, const TDesC* aDefault)
static TInt ShimParseSet(TUint8* aThis, const TDesC* aName, const TDesC* aRelated, const TDesC* aDefault)
    {
    TParse tp;
    const TInt r = tp.Set(*aName, aRelated, aDefault);
    if (r == KErrNone)
        {
        TFileName& nb = *(TFileName*)(aThis + KOldNameOff);
        nb.Copy(tp.FullName());
        }
    if (gLog && gParseLogs < 8)
        {
        gParseLogs++;
        TBuf8<160> shown;
        shown.Copy(tp.FullName().Left(shown.MaxLength()));
        gLog->Logf(_L8("fs: TParse::Set -> %d, full name '%S'"), r, &shown);
        }
    return r;
    }

// DriveAndPath() of the name stored in the old object, as a TPtrC that points INTO the old
// object's own buffer (a pointer into a temporary TParse would dangle).
static TPtrC DriveAndPathOf(const TUint8* aThis)
    {
    const TFileName& nb = *(const TFileName*)(aThis + KOldNameOff);
    TParse tp;
    if (nb.Length() == 0 || tp.Set(nb, NULL, NULL) != KErrNone)
        {
        return TPtrC();
        }
    const TPtrC part(tp.DriveAndPath());
    const TDesC& full = tp.FullName();
    if (full.Length() != nb.Length())
        {
        return TPtrC();
        }
    const TInt off = part.Ptr() - full.Ptr();
    return TPtrC(nb.Ptr() + off, part.Length());
    }

// efsrv 33: TPtrC TParseBase::DriveAndPath() const
// Verified at all 7 call sites of the binary: `this` in r0, and the 8-byte TPtrC comes back in
// r0:r1 (iLength, iPtr) -- the caller does "stm r3,{r0,r1}". No hidden result pointer.
// In EABI a TUint64 result is also returned in r0 (low) : r1 (high), so this matches.
static TInt gDpLogs = 0;

static TUint64 ShimParseDriveAndPath(TUint32 aThis)
    {
    const TBool known = IsParse(aThis);
    if (gLog && gDpLogs < 4)
        {
        gDpLogs++;
        gLog->Logf(_L8("fs: DriveAndPath this=%08x facade=%d"), aThis, known);
        }
    if (!known)
        {
        return 0;                               // empty TPtrC
        }
    const TPtrC part = DriveAndPathOf((const TUint8*)aThis);
    const TUint32* w = (const TUint32*)&part;   // iLength (with type bits), iPtr
    return ((TUint64)w[1] << 32) | (TUint64)w[0];
    }

// apparc 3: TFileName CApaApplication::AppFullName() const -- result through hidden pointer in r0
static TAny* ShimAppFullName(TUint8* aRet, TAny* /*aApp*/)
    {
    TFileName* r = new (aRet) TFileName;
    r->Copy(gAppFile);
    return aRet;
    }

// ---------------------------------------------------------------------------
// RFs / RFile / handles through the real v3 API
// ---------------------------------------------------------------------------
static TInt gOpLogs[5];                              // Open, Replace, Read, Write, Size
const TInt KOpLogMax = 4;

static TBool LogOp(TInt aOp)
    {
    if (gLog && gOpLogs[aOp] < KOpLogMax)
        {
        gOpLogs[aOp]++;
        return ETrue;
        }
    return EFalse;
    }

static TInt ShimFsConnect(RFs* aThis, TInt aSlots)                                  // efsrv 18
    {
    return aThis->Connect(aSlots);
    }

static TInt ShimFileOpen(RFile* aThis, RFs* aFs, const TDesC* aName, TUint aMode)   // efsrv 121
    {
    TFileName mapped;
    const TDesC* nm = aName;
    if (RemapOldPath(*aName, mapped))
        {
        LogRemap(*aName, mapped);
        nm = &mapped;
        }
    const TInt r = aThis->Open(*aFs, *nm, aMode);
	
	if (LogOp(0))
        {
        TBuf8<160> n;
        n.Copy(aName->Left(n.MaxLength()));
        gLog->Logf(_L8("fs: Open('%S', mode %x) -> %d"), &n, aMode, r);
        }
    return r;
    }

static TInt ShimFileReplace(RFile* aThis, RFs* aFs, const TDesC* aName, TUint aMode) // efsrv 151
    {
    TFileName mapped;
    const TDesC* nm = aName;
    if (RemapOldPath(*aName, mapped))
        {
        LogRemap(*aName, mapped);
        nm = &mapped;
        }
    const TInt r = aThis->Replace(*aFs, *nm, aMode);
    if (LogOp(1))
        {
        TBuf8<160> n;
        n.Copy(aName->Left(n.MaxLength()));
        gLog->Logf(_L8("fs: Replace('%S', mode %x) -> %d"), &n, aMode, r);
        }
    return r;
    }

static TInt ShimFileRead(RFile* aThis, TDes8* aDes)                                 // efsrv 136
    {
    const TInt maxLen = aDes->MaxLength();
    const TInt r = aThis->Read(*aDes);
	TBuf8<100> t;
    t.Format(_L8("Read max %d -> %d, got %d"), maxLen, r, aDes->Length());
    if (LogOp(2))
        {
        gLog->Logf(_L8("fs: Read(max %d) -> %d, got %d bytes"), maxLen, r, aDes->Length());
        }
    return r;
    }

static TInt ShimFileWrite(RFile* aThis, const TDesC8* aDes)                         // efsrv 194
    {
    const TInt r = aThis->Write(*aDes);
    if (LogOp(3))
        {
        gLog->Logf(_L8("fs: Write(%d bytes) -> %d"), aDes->Length(), r);
        }
    return r;
    }

static TInt ShimFileSize(RFile* aThis, TInt* aSize)                                 // efsrv 185
    {
    const TInt r = aThis->Size(*aSize);
    if (LogOp(4))
        {
        gLog->Logf(_L8("fs: Size -> %d, size %d"), r, (r == KErrNone) ? *aSize : 0);
        }
    return r;
    }

static TInt gMoreFsLogs = 0;

static TInt ShimFileCreate(RFile* aThis, RFs* aFs, const TDesC* aName, TUint aMode)   // efsrv 25
    {
    const TInt r = aThis->Create(*aFs, *aName, aMode);
    if (gLog && gMoreFsLogs < 12)
        {
        gMoreFsLogs++;
        TBuf8<160> n;
        n.Copy(aName->Left(n.MaxLength()));
        gLog->Logf(_L8("fs: Create('%S', mode %x) -> %d"), &n, aMode, r);
        }
    return r;
    }

static TInt ShimFsDelete(RFs* aThis, const TDesC* aName)                              // efsrv 29
    {
    const TInt r = aThis->Delete(*aName);
    if (gLog && gMoreFsLogs < 12)
        {
        gMoreFsLogs++;
        TBuf8<160> n;
        n.Copy(aName->Left(n.MaxLength()));
        gLog->Logf(_L8("fs: Delete('%S') -> %d"), &n, r);
        }
    return r;
    }

static TInt ShimFileFlush(RFile* aThis)                                               // efsrv 50
    {
    return aThis->Flush();
    }

static TInt ShimFsRename(RFs* aThis, const TDesC* aOld, const TDesC* aNew)            // efsrv 146
    {
    const TInt r = aThis->Rename(*aOld, *aNew);
    if (gLog && gMoreFsLogs < 12)
        {
        gMoreFsLogs++;
        gLog->Logf(_L8("fs: Rename -> %d"), r);
        }
    return r;
    }

// efsrv 188: RFile::Temp(RFs&, const TDesC& aPath, TFileName& aName, TUint aMode). TFileName is
// TBuf<256> in both SDKs (8 + 512 bytes), so the old buffer can be used as it is.
static TInt ShimFileTemp(RFile* aThis, RFs* aFs, const TDesC* aPath, TDes16* aName, TUint aMode)
    {
    const TInt r = aThis->Temp(*aFs, *aPath, *(TFileName*)aName, aMode);
    if (gLog && gMoreFsLogs < 12)
        {
        gMoreFsLogs++;
        gLog->Logf(_L8("fs: Temp -> %d"), r);
        }
    return r;
    }

// avkon 293: TInt CompleteWithAppPath(TDes16& aFileName). On an S60v2 phone it completes a file name
// with the drive and path of the application. Candy passes an EMPTY name and appends its own relative
// names to the result, so "" must become the application directory WITH the trailing backslash.
static TInt ShimCompleteWithAppPath(TDes16* aName)
    {
    TParse app;
    if (app.Set(gAppFile, NULL, NULL) != KErrNone)
        {
        return KErrNone;
        }
    const TDesC& dp = app.DriveAndPath();           // "E:\symbridge\Candy\"
    TFileName out;
    const TInt len = aName->Length();
    if (len == 0)
        {
        out.Copy(dp);
        }
    else if (len >= 2 && (*aName)[1] == ':')
        {
        out.Copy(*aName);                           // has a drive already
        }
    else if ((*aName)[0] == '\\')
        {
        out.Copy(dp.Left(2));                       // only the drive is missing
        out.Append(*aName);
        }
    else
        {
        out.Copy(dp);
        out.Append(*aName);
        }
    aName->Copy(out.Left(aName->MaxLength()));
    if (gLog && gMoreFsLogs < 12)
        {
        gMoreFsLogs++;
        TBuf8<160> n;
        n.Copy(aName->Left(n.MaxLength()));
        gLog->Logf(_L8("fs: CompleteWithAppPath -> '%S'"), &n);
        }
    return KErrNone;
    }

static void ShimFileClose(RFile* aThis)             // efsrv 15 = RFsBase::Close() = RFile::Close()
    {
	CRUMB(7, aThis, 0, __builtin_return_address(0));
    aThis->Close();
    }

static void ShimHandleClose(RHandleBase* aThis)     // euser 172 (RFs, RSemaphore, RMutex, RThread ...)
    {
    CRUMB(8, aThis, 0, __builtin_return_address(0));
	aThis->Close();
    }

//---------------------------------------------------------------------------
// Directory listing (needed by Atomanic). CDir is a v3 class with a virtual destructor, and the old code
// deletes it THROUGH ITS VTABLE (GCC 2.9 layout: destructor at word 2). The v3 vtable is laid out
// differently, so the old code never sees the real CDir: it gets a small facade {old vptr, real CDir*}
// whose destructor deletes the real one. CDir::Count and CDir::operator[] unwrap it. TEntry has the same
// public layout (iAtt, iSize, iModified, iType, iName) in v2 and v3, so entries are returned as they are.
// ---------------------------------------------------------------------------
struct TFakeDir
    {
    TUint32 iVptr;
    CDir* iReal;
    };

static TUint32 gDirVt[8];
static TInt gDirLogs = 0;

static void DirFakeDtor(TFakeDir* aThis, TInt aFlags)
    {
    delete aThis->iReal;
    aThis->iReal = NULL;
    if (aFlags & 1)
        {
        User::Free(aThis);
        }
    }

static TFakeDir* WrapDir(CDir* aReal)
    {
    if (!aReal)
        {
        return NULL;
        }
    TFakeDir* d = (TFakeDir*)User::AllocZ(sizeof(TFakeDir));
    if (!d)
        {
        delete aReal;
        return NULL;
        }
    d->iVptr = (TUint32)gDirVt;
    d->iReal = aReal;
    return d;
    }

// efsrv 58: TInt RFs::GetDir(const TDesC&, TUint aAttMask, TUint aSortKey, CDir*& aFiles, CDir*& aDirs) const
static TInt ShimFsGetDir(RFs* aThis, const TDesC* aName, TUint aAtt, TUint aSort, CDir** aFiles, CDir** aDirs)
    {
    CDir* files = NULL;
    CDir* dirs = NULL;
    const TInt r = aThis->GetDir(*aName, aAtt, aSort, files, dirs);
    const TInt nf = files ? files->Count() : 0;
    const TInt nd = dirs ? dirs->Count() : 0;
    *aFiles = (CDir*)WrapDir(files);
    *aDirs = (CDir*)WrapDir(dirs);
    if (gLog && gDirLogs < 4)
        {
        gDirLogs++;
        TBuf8<120> n;
        n.Copy(aName->Left(n.MaxLength()));
        gLog->Logf(_L8("fs: GetDir('%S', att %x, sort %x) -> %d, %d files, %d dirs"), &n, aAtt, aSort, r, nf, nd);
        }
    return r;
    }

static TInt ShimCDirCount(const TFakeDir* aThis)                                   // efsrv 24
    {
    return aThis->iReal->Count();
    }

static const TAny* ShimCDirIndex(const TFakeDir* aThis, TInt aIndex)               // efsrv 1
    {
    return &(*aThis->iReal)[aIndex];
    }

// TParseBase::AddDir / PopDir work on the facade object: rebuild a real TParse from the stored name,
// apply the operation, store the new full name back.
static TInt ShimParseAddDir(TUint8* aThis, const TDesC* aDir)                       // efsrv 3
    {
    if (!IsParse((TUint32)aThis))
        {
        return KErrArgument;
        }
    TFileName& nb = *(TFileName*)(aThis + KOldNameOff);
    TParse tp;
    TInt r = tp.Set(nb, NULL, NULL);
    if (r == KErrNone)
        {
        r = tp.AddDir(*aDir);
        }
    if (r == KErrNone)
        {
        nb.Copy(tp.FullName());
        }
    return r;
    }

static TInt ShimParsePopDir(TUint8* aThis)                                          // efsrv 128
    {
    if (!IsParse((TUint32)aThis))
        {
        return KErrArgument;
        }
    TFileName& nb = *(TFileName*)(aThis + KOldNameOff);
    TParse tp;
    TInt r = tp.Set(nb, NULL, NULL);
    if (r == KErrNone)
        {
        r = tp.PopDir();
        }
    if (r == KErrNone)
        {
        nb.Copy(tp.FullName());
        }
    return r;
    }

// RFile reads/writes with an explicit length or position (same RFile layout in both SDKs)
static TInt ShimFileReadLen(RFile* aThis, TDes8* aDes, TInt aLen)                   // efsrv 138
    {
    const TInt r = aThis->Read(*aDes, aLen);
    if (gLog && gDiagLogs[0] < 6)
        {
        gDiagLogs[0]++;
        gLog->Logf(_L8("fs: Read(len %d) -> %d, got %d bytes"), aLen, r, aDes->Length());
        }
    return r;
    }

static TInt ShimFileReadAt(RFile* aThis, TInt aPos, TDes8* aDes, TInt aLen)         // efsrv 142
    {
    const TInt r = aThis->Read(aPos, *aDes, aLen);
    if (gLog && gDiagLogs[1] < 6)
        {
        gDiagLogs[1]++;
        gLog->Logf(_L8("fs: Read(pos %d, len %d) -> %d, got %d bytes"), aPos, aLen, r, aDes->Length());
        }
    return r;
    }

static TInt ShimFileSeek(RFile* aThis, TInt aMode, TInt* aPos)                      // efsrv 162
    {
    const TInt before = *aPos;
    const TInt r = aThis->Seek((TSeek)aMode, *aPos);
    if (gLog && gDiagLogs[2] < 6)
        {
        gDiagLogs[2]++;
        gLog->Logf(_L8("fs: Seek(mode %d, pos %d) -> %d, new pos %d"), aMode, before, r, *aPos);
        }
    return r;
    }

static TInt ShimFileWriteLen(RFile* aThis, const TDesC8* aDes, TInt aLen)           // efsrv 196
    {
    return aThis->Write(*aDes, aLen);
    }

static TInt ShimSetSessionPath(RFs* aThis, const TDesC* aPath)                      // efsrv 178
    {
    return aThis->SetSessionPath(*aPath);
    }

// ---------------------------------------------------------------------------
// stdio: fopen (estlib 58)
// ---------------------------------------------------------------------------
typedef void* (*TRealFopen)(const char*, const char*);
static RLibrary gEstlib;
static TRealFopen gRealFopen = NULL;
static TInt gFopenLogs = 0;

static void ResolveRealFopen()
    {
    if (gRealFopen)
        {
        return;                                 // resolved already: do not load the library again
        }
    TInt err = KErrNotFound;
    TUint32 ord = 0;
    const TImportMapEntry* e = ImportMapFind(_L8("estlib"), 58);
    if (e && e->iKind == EDirect && e->iNew != 0)
        {
        ord = e->iNew;
        err = gEstlib.Load(_L("estlib.dll"));
        if (err == KErrNone)
            {
            gRealFopen = (TRealFopen)(TUint32)gEstlib.Lookup(ord);
            }
        }
    if (gLog)
        {
        gLog->Logf(_L8("fs: real fopen = %08x (estlib ordinal %d, load %d)"), (TUint32)gRealFopen, (TInt)ord, err);
        }
    }

// The name a game gives fopen -> a real path in the game folder:
  // "scores.dat", "sounds/cn_hit.wav"   (relative)                  -> <game folder>\...
  // "\crash.txt"                        (drive root, no folder)     -> <game folder>\crash.txt
  // "X:\system\apps\..." or "\system\apps\..."                      -> the same remap as the RFs paths
  // any other absolute path                                         -> unchanged
// '/' is turned into '\'.
static void MapCName(const char* aIn, TDes8& aOut)
    {
    TPtrC8 in((const TUint8*)aIn);
    TFileName name;
    if (in.Length() > name.MaxLength())
        {
        aOut.Copy(in.Left(aOut.MaxLength()));
        return;
        }
    name.Copy(in);
    for (TInt i = 0; i < name.Length(); i++)
        {
        if (name[i] == '/')
            {
            name[i] = '\\';
            }
        }
    TFileName mapped;
    TBool done = EFalse;
    if (name.Length() >= 2 && name[1] == ':')
        {
        done = RemapOldPath(name, mapped);
        }
    else if (name.Length() >= 1 && name[0] == '\\')
        {
        TFileName withDrive;
        if (gAppRoot.Length() >= 2)
            {
            withDrive.Copy(gAppRoot.Left(2));
            }
        if (withDrive.Length() + name.Length() <= withDrive.MaxLength())
            {
            withDrive.Append(name);
            done = RemapOldPath(withDrive, mapped);
            }
        if (!done && name.Mid(1).Locate('\\') < 0 && gAppRoot.Length() + name.Length() - 1 <= mapped.MaxLength())
            {
            mapped.Copy(gAppRoot);
            mapped.Append(name.Mid(1));
            done = ETrue;
            }
        }
    else if (name.Length() > 0 && gAppRoot.Length() + name.Length() <= mapped.MaxLength())
        {
        mapped.Copy(gAppRoot);
        mapped.Append(name);
        done = ETrue;
        }
    if (done)
        {
        aOut.Copy(mapped.Left(aOut.MaxLength()));
        }
    else
        {
        aOut.Copy(name.Left(aOut.MaxLength()));
        }
    }

static void* ShimFopen(const char* aName, const char* aMode)                       // estlib 58
    {
    if (!gRealFopen || !aName || !aMode)
        {
        static TInt noFopenLogs = 0;            // a silent NULL looks like "file not found" to the game
        if (gLog && noFopenLogs < 3)
            {
            noFopenLogs++;
            gLog->Logf(_L8("fs: fopen called but the real function is not available (estlib not loaded or import map row missing)"));
            }
        return NULL;
        }
    TBuf8<KMaxFileName + 1> path;
    MapCName(aName, path);
	
	path.ZeroTerminate();
	CRUMB(19, path.Ptr(), 0, 0);                       // before the real fopen
    void* f = gRealFopen((const char*)path.Ptr(), aMode);
	CRUMB(20, f, 0, 0);                                // after it
    if (gLog && gFopenLogs < 14)
        {
        gFopenLogs++;
        TPtrC8 a((const TUint8*)aName);
        TPtrC8 m((const TUint8*)aMode);
        TBuf8<100> from, to;
        from.Copy(a.Left(from.MaxLength()));
        to.Copy(path.Left(to.MaxLength()));
        gLog->Logf(_L8("fs: fopen '%S' mode '%S' -> '%S' = %08x"), &from, &m, &to, (TUint32)f);
        }
    return f;
    }

static const TShimRegEntry KFsShimEntries[] =
    {
    { "efsrv", 1,   (TUint32)&ShimCDirIndex, 0, "fsshim", NULL },
    { "efsrv", 3,   (TUint32)&ShimParseAddDir, 0, "fsshim", NULL },
    { "efsrv", 15,  (TUint32)&ShimFileClose, 0, "fsshim", NULL },
    { "efsrv", 18,  (TUint32)&ShimFsConnect, 0, "fsshim", NULL },
    { "efsrv", 33,  (TUint32)&ShimParseDriveAndPath, 0, "fsshim", NULL },
    { "efsrv", 121, (TUint32)&ShimFileOpen, 0, "fsshim", NULL },
    { "efsrv", 136, (TUint32)&ShimFileRead, 0, "fsshim", NULL },
    { "efsrv", 24,  (TUint32)&ShimCDirCount, 0, "fsshim", NULL },
    { "efsrv", 58,  (TUint32)&ShimFsGetDir, 0, "fsshim", NULL },
    { "efsrv", 128, (TUint32)&ShimParsePopDir, 0, "fsshim", NULL },
    { "efsrv", 138, (TUint32)&ShimFileReadLen, 0, "fsshim", NULL },
    { "efsrv", 142, (TUint32)&ShimFileReadAt, 0, "fsshim", NULL },
    { "efsrv", 162, (TUint32)&ShimFileSeek, 0, "fsshim", NULL },
    { "efsrv", 178, (TUint32)&ShimSetSessionPath, 0, "fsshim", NULL },
    { "efsrv", 151, (TUint32)&ShimFileReplace, 0, "fsshim", NULL },
    { "efsrv", 183, (TUint32)&ShimParseSet, 0, "fsshim", NULL },
    { "efsrv", 185, (TUint32)&ShimFileSize, 0, "fsshim", NULL },
    { "efsrv", 194, (TUint32)&ShimFileWrite, 0, "fsshim", NULL },
    { "efsrv", 196, (TUint32)&ShimFileWriteLen, 0, "fsshim", NULL },
    { "efsrv", 230, (TUint32)&ShimParseCtor, 0, "fsshim", NULL },
    { "efsrv", 25,  (TUint32)&ShimFileCreate, 0, "fsshim", NULL },
    { "efsrv", 29,  (TUint32)&ShimFsDelete, 0, "fsshim", NULL },
    { "efsrv", 50,  (TUint32)&ShimFileFlush, 0, "fsshim", NULL },
    { "efsrv", 146, (TUint32)&ShimFsRename, 0, "fsshim", NULL },
	{ "estlib", 58, (TUint32)&ShimFopen, 0, "fsshim", NULL },
	{ "efsrv", 188, (TUint32)&ShimFileTemp, 0, "fsshim", NULL },
    { "avkon", 293, (TUint32)&ShimCompleteWithAppPath, 0, "fsshim", NULL },
    { "apparc", 3,  (TUint32)&ShimAppFullName, 0, "fsshim", NULL },
    { "euser", 172, (TUint32)&ShimHandleClose, 0, "fsshim", NULL }
    };

TInt FsShimReg_Init()
    {
    return ShimReg_Add(KFsShimEntries, sizeof(KFsShimEntries) / sizeof(KFsShimEntries[0]));
    }

// ---------------------------------------------------------------------------
void FsShim_Init(THostLog* aLog, const TDesC& aAppFullPath)
    {
    gLog = aLog;
	ResolveRealFopen();
    gDirVt[0] = 0;
    gDirVt[1] = 0;
    gDirVt[2] = (TUint32)&DirFakeDtor;
    for (TInt d = 3; d < 8; d++)
        {
        gDirVt[d] = (TUint32)&ParseVirtualStub;
        }

    gVtParse[0] = 0;
    gVtParse[1] = 0;
    for (TInt i = 2; i < 8; i++)
        {
        gVtParse[i] = (TUint32)&ParseVirtualStub;
        }
    gAppFile.Copy(aAppFullPath);
    TParse tp;
    if (tp.Set(gAppFile, NULL, NULL) == KErrNone)
        {
        gAppRoot.Copy(tp.DriveAndPath());
        }
    if (gLog)
        {
        TBuf8<160> shown;
        shown.Copy(gAppFile.Left(shown.MaxLength()));
        gLog->Logf(_L8("fs: AppFullName will answer '%S'"), &shown);
        }
    }

const TDesC& FsShim_AppPath()
    {
    return gAppFile;
    }
