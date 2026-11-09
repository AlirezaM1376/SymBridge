// File-system side of the compatibility layer for the old game.
//  - TParse facade (old layout: vptr, iWild, 4 x SField, TFileName at +24, 544 bytes)
//  - CApaApplication::AppFullName() answers the real path the .app was loaded from, so the
//    game finds its own data files next to it; old absolute install paths (\system\apps\<game>\...)
//    are mapped into the game's folder
//  - RFs / RFile / RHandleBase::Close forwarded through the real v3 C++ API, not through
//    ordinals; the first results of Open/Replace/Read/Write/Size are logged
#ifndef FSSHIM_H
#define FSSHIM_H

#include <e32std.h>
#include "hostlog.h"

// aAppFullPath: full path of the loaded .app, e.g. E:\symbridge\captaindynamo\CaptainDynamo.app
void FsShim_Init(THostLog* aLog, const TDesC& aAppFullPath);
TInt FsShimReg_Init();
TBool FsShim_RemapOldPath(const TDesC& aName, TFileName& aOut);   // EFalse = the name is left alone
const TDesC& FsShim_AppPath();                          // full path the .app was loaded from

#endif
