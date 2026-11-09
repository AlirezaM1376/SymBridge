// Threads, semaphores and mutexes of the old game on top of the real v3 API.
//
//  - RThread::Create (old signature) creates a real v3 thread, suspended, with a NULL heap
//    (it shares the creator's heap, so TOldRt::Shutdown may free any thread's state), a minimum
//    stack of 0x10000 and a unique name. The old function runs from a trampoline.
//  - Trampoline order (State() panics OLDRT 2 otherwise):
//      CTrapCleanup::New -> TOldRt::AttachThread -> TRAP(old code) -> DetachThread -> delete cleanup
//  - RThread::SetInitialParameter (removed in v3) stores the parameter in the pending context.
//  - Old TThreadPriority values (-30..30, 100..500) are translated to the v3 enumerators.
//  - RThread::RaiseException / SetExceptionHandler (removed in v3) are emulated: the game thread owns an
//    active object that the raising thread completes; its RunL calls the old handler in the game thread.
//  - Dll::SetTls / Tls / FreeTls are kept per thread; CTrapCleanup::New answers with a fake object.
//  - RSemaphore::Wait called by a non-game thread is bounded (see KUiWaitCapMicros) so a
//    thread that dies cannot freeze the UI forever.
#ifndef THREADSHIM_H
#define THREADSHIM_H

#include <e32std.h>
#include "hostlog.h"

void ThreadShim_Init(THostLog* aLog);
TInt ThreadShimReg_Init();
void ThreadShim_SetPolicy(TInt aMinStack, TInt aHighPriorityAs);   // from the game profile
// The exit trick of ThreadShim_StopAndWait, from the game profile (stop_state_tls / _field / _value, see
// profile.h). aTlsOffset 0 = off.
void ThreadShim_SetStopState(TInt aTlsOffset, TInt aFieldOffset, TInt aValue);
TBool ThreadShim_StopAndWait(TInt aMaxMs);          // applies the exit trick, re-signals the game threads and waits; ETrue = none left
void ThreadShim_Shutdown();                         // stops game threads still running

#endif
