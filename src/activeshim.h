// Derived CActive classes of old (pre-Symbian-9) games, and RThread::RequestComplete.
//
// An old game class that derives from CActive owns the memory of its object; the real v3 active
// object lives here, in a side table (the same idea as the derived-CIdle objects of schedshim.cpp):
//   old object  <->  CGameActive (a real v3 CActive) -- registered with the scheduler of the thread
//                    that constructed it; its RunL / DoCancel / RunError call the game's own
//                    virtual functions through TOldRt (old code is only ever entered via TOldRt).
// RThread::RequestComplete on the OLD object's iStatus (offset 4) completes the REAL iStatus, so the
// real active scheduler of the owning thread wakes up. This works across threads.
//
// Limits: only requests that are completed by RThread::RequestComplete are supported. A game that
// passes its old iStatus to a real asynchronous API (a timer, a socket ...) needs a dedicated shim.
#ifndef ACTIVESHIM_H
#define ACTIVESHIM_H

#include <e32std.h>
#include "hostlog.h"

void ActiveShim_Init(THostLog* aLog);
TInt ActiveShimReg_Init();

// Used by schedshim.cpp for the two ordinals it already owns. ETrue = aOld is a derived CActive
// and the call was handled here.
TBool ActiveShim_Add(TAny* aOld);          // euser 13:  CActiveScheduler::Add(CActive*)
TBool ActiveShim_Cancel(TAny* aOld);       // euser 152: CActive::Cancel()

#endif
