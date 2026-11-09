#include "crashwatch.h"
#include <f32file.h>

struct TWatchArg
    {
    TThreadId iMain;
    TFileName iPath;
    };
static TWatchArg gWatchArg;

TInt gLogLevel = 1;
struct TCrumb
    {
    TUint32 iTick, iTag, iA, iB, iLr;
    };
static TCrumb gCrumbs[64];
static volatile TUint32 gCrumbNext = 0;

static TCrumb gLastByTag[32];

void CrashCrumb(TUint32 aTag, TUint32 aA, TUint32 aB, TUint32 aLr)
    {
	TCrumb& last = gLastByTag[aTag & 31]; // tags must stay below 32.
    if (last.iTag == aTag && last.iA == aA && last.iB == aB && last.iLr == aLr)
        {
        return;                                     // same call again (the sound mixer repeats one every 50 ms): don't flood the ring
        }
    last.iTag = aTag;
    last.iA = aA;
    last.iB = aB;
    last.iLr = aLr;
    TCrumb& c = gCrumbs[(gCrumbNext++) & 63];  
	c.iTick = (gLogLevel >= 2) ? User::NTickCount() : 0;
    c.iTag  = aTag | ((gLogLevel >= 2) ? ((TUint32)RThread().Id() << 8) : 0); // low byte tag, rest = thread id
    c.iA = aA;
    c.iB = aB;
    c.iLr = aLr;
    }

static void AppendLine(RFs& aFs, const TDesC& aPath, const TDesC8& aLine)
    {
    RFile f;
    TInt err = f.Open(aFs, aPath, EFileWrite | EFileShareAny);
    if (err != KErrNone)
        {
        // the log may be opened exclusively: write to a side file instead
        TFileName alt(aPath);
        alt.Append(_L(".crash"));
        err = f.Replace(aFs, alt, EFileWrite | EFileShareAny);
        }
    if (err == KErrNone)
        {
        TInt pos = 0;
        f.Seek(ESeekEnd, pos);
        f.Write(aLine);
        f.Flush();
        f.Close();
        }
    }

static TInt WatchFn(TAny*)
    {
    RThread main;
    if (main.Open(gWatchArg.iMain) != KErrNone)
        {
        return KErrNotFound;
        }
    TRequestStatus st;
    main.Logon(st);
    User::WaitForRequest(st);               // returns when the UI thread has died
    RFs fs;
    if (fs.Connect() != KErrNone)
        {
        return KErrGeneral;
        }
    TExitCategoryName cat = main.ExitCategory();
    TBuf8<32> cat8;
    cat8.Copy(cat);
    gHostExitKind.Copy(_L8("panic"));
    TBuf8<128> line;
    line.Format(_L8("CRASHWATCH: UI thread exit type=%d (0=kill 1=terminate 2=panic) category=%S reason=%d\r\n"),
                (TInt)main.ExitType(), &cat8, main.ExitReason());
    AppendLine(fs, gWatchArg.iPath, line);
	 
	const TUint32 total = (gLogLevel > 0) ? gCrumbNext : 0;
    const TUint32 first = total > 64 ? total - 64 : 0;
    for (TUint32 n = first; n < total; n++)
        {
        const TCrumb& c = gCrumbs[n & 63];
        line.Format(_L8("CRUMB %u: tick %u tag %u thread %x a=%08x b=%08x lr=%08x\r\n"),
                    n, c.iTick, c.iTag & 0xFF, c.iTag >> 8, c.iA, c.iB, c.iLr);
		AppendLine(fs, gWatchArg.iPath, line);
        }

    fs.Close();
    // The UI thread is gone: end the whole process, or the game and audio threads
    // keep it alive in the background.
    RProcess().Kill(main.ExitReason());
    return KErrNone;
    }

TInt StartCrashWatch(const TDesC& aLogPath)
    {
    gWatchArg.iMain = RThread().Id();
    gWatchArg.iPath = aLogPath;
    RThread t;
    TInt err = t.Create(_L("CrashWatch"), WatchFn, 0x10000, NULL, &gWatchArg);
    if (err == KErrNone)
        {
        t.Resume();
        t.Close();
        }
		
    // The main thread is process-permanent by default, so the process would die with it
    // before the watcher can write. Try to lift that.
    User::SetCritical(User::ENotCritical);
    return err;
    }

// ---------------------------------------------------------------------------
// Exit watchdog
// ---------------------------------------------------------------------------
static TInt gWatchdogMillis = 0;

static TInt WatchdogFn(TAny*)
    {
    User::After(gWatchdogMillis * 1000);
    RFs fs;
    if (fs.Connect() == KErrNone)
        {
        gHostExitKind.Copy(_L8("watchdog"));
        TBuf8<128> line;
        line.Format(_L8("EXITWATCHDOG: teardown did not finish within %d ms, killing the process\r\n"), gWatchdogMillis);
        AppendLine(fs, gWatchArg.iPath, line);
        fs.Close();
        }
    RProcess().Kill(KErrTimedOut);
    return KErrNone;
    }

void StartExitWatchdog(TInt aMillis)
    {
    if (gWatchdogMillis != 0)
        {
        return;                                 // already armed
        }
    gWatchdogMillis = aMillis;
    RThread t;
    if (t.Create(_L("ExitWatchdog"), WatchdogFn, 0x10000, NULL, &gWatchArg) == KErrNone)
        {
        t.Resume();
        t.Close();
        }
    }
