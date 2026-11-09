// Feasibility test for the "Leave" problem.
//
// Old (pre-Symbian-9) code has no C++ unwind tables and implements Leave with
// setjmp/longjmp-style TTrap. Symbian 9 implements Leave as a C++ exception
// (XLeaveException). The test checks the planned bridge:
//   new->old: a shim catches the new-style leave and does an old-style jump
//   old->new: a trampoline turns an old-style jump back into a real leave
// "Old" frames are simulated with hand-assembled ARM code in a code chunk
// (no unwind info), see leavetest.cpp.
#ifndef LEAVETEST_H
#define LEAVETEST_H

#include <e32base.h>
#include <e32std.h>
#include "hostlog.h"

class CLeaveTest : public CBase
    {
public:
    static CLeaveTest* NewL(THostLog& aLog);
    ~CLeaveTest();
    void RunSafe();              // T1..T3: should not kill the process
    void RunCleanupTests();      // T5: cleanup stack across the bridge
    void RunUnwindCrashTest();   // T4: kills the process (result already known); not run by default
private:
    CLeaveTest(THostLog& aLog) : iLog(aLog) {}
    void ConstructL();
    THostLog& iLog;
    RChunk iChunk;
    };

#endif
