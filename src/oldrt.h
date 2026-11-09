// Runtime support for running pre-Symbian-9 ("old") code inside this process.
// Same mechanism as the leave tests (setjmp/longjmp style old traps), but as a
// reusable module. Old code has no unwind info, so it must only ever be entered
// through TOldRt::Call0/Call1 and must only be left through TOldRt::Leave.
//
// All mutable state (trap chain, cleanup depth, result slot, flags) is PER THREAD.
// Every thread that runs old code must call TOldRt::AttachThread() after its
// CTrapCleanup exists, and TOldRt::DetachThread() before it ends.
// TOldRt::Init() attaches the calling thread itself.
#ifndef OLDRT_H
#define OLDRT_H

#include <e32base.h>
#include <e32std.h>

struct TOldTrapFrame
    {
    TInt iBuf[10];                  // r4-r11, sp, lr
    TOldTrapFrame* iPrev;
    volatile TInt iReason;
    TInt iDepth;                    // old-code cleanup depth at trap time
    TInt* iResultPtr;               // old TTrap::Trap(TInt&): where the leave code is stored (else NULL)
    TBool iHeap;                    // frame was made by the old TRAP stub (heap, freed by UnTrap/Leave)
    };

struct TOldThreadState
    {
    TUint64 iThreadId;
    TOldTrapFrame* iTrapTop;
    TInt iCleanupDepth;
    TInt iCallResult;
    TBool iInOldDelete;             // an old destructor runs during leave cleanup
    TInt iHit;                      // import index of the last trap/trace stub hit, or -1
    TBool iIsGameThread;            // thread started by the RThread::Create shim (not the UI thread)
    TAny* iTls;                     // old Dll::SetTls/Tls value of this thread
    };

class TOldRt
    {
public:
    // Creates the small helper code chunk (setjmp, longjmp, call helper), the state lock
    // and attaches the calling thread.
    static TInt Init();
    static void Shutdown();

    // Register / unregister the CURRENT thread (idempotent).
    static TInt AttachThread();
    static void DetachThread();

    // Snapshot of another thread's state (for the death watcher). KErrNotFound if that thread
    // already detached, which is the normal end of a game thread.
    static TInt ThreadInfo(TUint64 aThreadId, TInt& aHit, TInt& aCleanupDepth);

    // State of the current thread. Panics OLDRT 2 if the thread was never attached.
    static TOldThreadState& State();

    // Calls aFn() / aFn(aArg) under an old-style trap.
    // Returns KErrNone and the function's r0 in aResult, or the reason that
    // was passed to Leave().
    static TInt Call0(TAny* aFn, TInt& aResult);
    static TInt Call1(TAny* aFn, TInt aArg, TInt& aResult);
    // Same with up to four arguments (r0-r3), e.g. an old member function: this, a1, a2, a3.
    static TInt CallArgs(TAny* aFn, TInt a0, TInt a1, TInt a2, TInt a3, TInt& aResult);

    // Old-style leave: jumps back to the innermost Call0/Call1 OF THIS THREAD. Never returns.
    static void Leave(TInt aReason);

    // Like Leave, but cleanup items pushed by old code are only discarded
    // (Pop), not destroyed. For deliberate stops by the host.
    static void Abandon(TInt aReason);

    // Old-style TRAP (TTrap::Trap / TTrap::UnTrap imports). TrapStub() is the address of a
    // hand-assembled ARM routine that records the CALLER's registers (a setjmp in the caller's
    // frame) and returns 0; a later Leave resumes there with r0 = 1 and *aResult = reason.
    static TUint32 TrapStub();
    static TUint32 UnTrapFn();

    // For the cleanup-stack shims.
    static void NotePush();
    static void NotePop();
    };

// Shim registry table of this module (euser TTrap::Trap / TTrap::UnTrap). Call once at start-up.
TInt OldRtShim_Init();

#endif
