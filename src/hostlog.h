// Thread-safe file logger.
//
// The log owns its own file server session (shareable, so game threads can log too) and serialises
// writes with a lock. Every line is written with one call. The file is flushed at most every
// KFlushIntervalMs and when it is closed, so logging stays cheap; a process crash loses nothing
// (the file server already has the data), only a power loss can lose the last fraction of a second.
//
// Logging can be switched off (logging=off in configs.cfg, see hostconfig.h): nothing is formatted or
// written then. The crash-watch lines of crashwatch.cpp are written on their own and stay.
// Everything that runs often must be capped by count (the existing logs are), because even a cheap line
// costs a file server call.
#ifndef HOSTLOG_H
#define HOSTLOG_H

#include <e32std.h>
#include <f32file.h>

class THostLog
    {
public:
	THostLog() : iOpen(EFalse), iEnabled(ETrue), iLastFlush(0), iFlushEach(EFalse) {}

    // aFs is not used (the log has its own session); the argument stays so callers need no change.
    TInt Open(RFs& aFs, const TDesC& aFileName);
    
	void Close();
    void Logf(TRefByValue<const TDesC8> aFmt, ...);
    void Text(const TDesC8& aText);
    void SetEnabled(TBool aEnabled) { iEnabled = aEnabled; }
	void SetFlushEachLine(TBool aOn) { iFlushEach = aOn; }
    TBool Enabled() const { return iOpen && iEnabled; }
    TBool IsOpen() const { return iOpen; }

private:
    RFs iFs;
    RFile iFile;
    RFastLock iLock;
    TBool iOpen;
    TBool iEnabled;
    TUint32 iLastFlush;                 // NTickCount of the last Flush()
    TBool iFlushEach;
	};

#endif
