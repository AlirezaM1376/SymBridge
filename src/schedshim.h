// The active scheduler family of the old game on top of the real v3 classes:
//  - CActiveScheduler (ctor, Install, Start, Stop, Add) and CActive::Cancel
//  - CIdle (New, NewL, Start), also as a base class of a game class (ctor / dtor)
//  - CPeriodic (New, Start)
//  - User::After, with a 1 ms timer instead of the 15.6 ms tick
// An old object is only a handle: it points to (or is paired with) the real v3 object, and the callbacks
// the real object makes (IdleThunk, PeriodicThunk) run the game's code through TOldRt::Call1.
// Derived CActive classes are in activeshim.cpp.
#ifndef SCHEDSHIM_H
#define SCHEDSHIM_H

#include <e32std.h>
#include "hostlog.h"

void SchedShim_Init(THostLog* aLog);
TInt SchedShimReg_Init();

// The host registers a function that runs on the idle thread right after each old idle callback,
// in plain v3 frames. It may leave (the host uses it to perform a pending CEikAppUi::Exit()).
void SchedShim_SetAfterCallHook(void (*aHook)());

// 0 = unlimited; else the old idle callbacks (game frames) run at most aFps times per second.
void SchedShim_SetMaxFps(TInt aFps);

#endif
