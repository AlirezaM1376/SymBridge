// Finds the game to run. Layout:  <drive>:|symbridge|<any folder name>|<game>.app  (+ its data files).
// Drives are tried in the order E, F, C; the first drive whose |symbridge holds a game wins and the
// others are not looked at. Absent drives (no card) are skipped. With several game folders the file
// |symbridge|autostart (first non-comment line: folder name, or the UID3 as 8 hex digits) chooses;
// without it the first folder in alphabetical order is taken. Everything the game writes goes next to
// its .app, in its own folder.
#ifndef GAMELOCATOR_H
#define GAMELOCATOR_H

#include <e32std.h>
#include <f32file.h>
#include "hostlog.h"

struct TGameInfo
    {
    TFileName iAppPath;         // full path of the .app, e.g. E:|symbridge|captaindynamo|CaptainDynamo.app
    TFileName iRoot;            // E:|symbridge
    TUint32 iUid3;              // UID3 from the image header: names the profile
    TInt iCodeSize;             // image header code size (profile safeguard match_code_size)
    };

// KErrNone with aOut filled, or KErrNotFound.
TInt GameLocator_Find(RFs& aFs, THostLog& aLog, TGameInfo& aOut);

#endif
