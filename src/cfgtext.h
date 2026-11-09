// Helpers shared by the small text files the host reads: configs.cfg (hostconfig.cpp), the game
// profiles (profile.cpp) and \symbridge\autostart (gamelocator.cpp).
//
// Line format:   name = value   # comment      (blank lines and lines starting with # are skipped)
#ifndef CFGTEXT_H
#define CFGTEXT_H

#include <e32std.h>
#include <f32file.h>

// Reads a whole settings file (at most 8 KB; KErrTooBig otherwise). The caller deletes the result.
HBufC8* CfgText_ReadFileL(RFs& aFs, const TDesC& aPath);

// Leading and trailing blanks (space, tab, CR) removed.
TPtrC8 CfgText_Trim(const TDesC8& aText);

// Takes the next line (without the '\n') off the front of aRest. EFalse when aRest is empty.
TBool CfgText_NextLine(TPtrC8& aRest, TPtrC8& aLine);

// "name = value   # comment" -> trimmed name and value. EFalse for blank and comment lines and for
// lines without '='.
TBool CfgText_SplitLine(const TDesC8& aLine, TPtrC8& aName, TPtrC8& aValue);

// Decimal or 0x.. hexadecimal.
TBool CfgText_ParseNumber(const TDesC8& aValue, TInt& aOut);

#endif
