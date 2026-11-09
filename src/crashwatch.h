// Logs how the UI thread dies (panic category / reason) from a helper thread.
#ifndef CRASHWATCH_H
#define CRASHWATCH_H
#include <e32std.h>
extern TBuf8<16> gHostExitKind;

// Tags (keep this list in sync with docs\BRINGING_UP_A_GAME.md, "Reading a crash"):
//  level 1: 10 present start, 11 present end, 12/14/23 container frame steps, 13/15 scale buffers,
//           16/17/18 failed or big allocation, 19/20 fopen before/after, 21/22 game frame enter/return,
//           7/8 RFile / handle Close
//  level 2: 1 Mem::FillZ, 2 Mem::Fill, 3 Mem::Copy, 4 __builtin_delete, 5 CBase::newL, 6 User::AllocL
//  (the macros evaluate the arguments only when the level allows, so __builtin_return_address costs nothing at level 0/1)
extern TInt gLogLevel;          // 0 = nothing recorded, 1 = cheap crumbs, 2 = all (set from configs.cfg logging=)
void CrashCrumb(TUint32 aTag, TUint32 aA, TUint32 aB, TUint32 aLr);   // do not call directly: use the macros
#define CRUMB(tag, a, b, lr)      do { if (gLogLevel >= 1) CrashCrumb((tag), (TUint32)(a), (TUint32)(b), (TUint32)(lr)); } while (0)
#define CRUMB_HOT(tag, a, b, lr)  do { if (gLogLevel >= 2) CrashCrumb((tag), (TUint32)(a), (TUint32)(b), (TUint32)(lr)); } while (0)

// aLogPath: the log file to append to. Call once from the UI thread.
TInt StartCrashWatch(const TDesC& aLogPath);

// Exit watchdog: if the process is still alive aMillis after this call, log it and kill the process.
// A frozen teardown can then never leave a hung or background app behind. Call at the start of exit.
void StartExitWatchdog(TInt aMillis);
#endif
