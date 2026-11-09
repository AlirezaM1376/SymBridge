// The old CMdaAudioOutputStream. The game calls CMdaAudioOutputStream::NewL (the only import
// of mediaclientaudiostream) and then uses the object through its VIRTUAL functions, so the old
// code needs an object with an old-style (GCC 2.9) vtable. The old object is a facade; a real v3
// CMdaAudioOutputStream does the playing and drives the callbacks. If it cannot be created or fails,
// the facade falls back to a silent, correctly paced fake:
//   Open        -> MaoscOpenComplete(KErrNone) shortly after
//   WriteL(buf) -> MaoscBufferCopied(KErrNone, buf) after the buffer's playing time
//   queue empty -> MaoscPlayComplete(KErrUnderflow)      Stop() -> MaoscPlayComplete(KErrCancel)
// Callbacks are old code: delivered in the creating thread's active scheduler through
// TOldRt::CallArgs (old trap, per-thread state).
#ifndef AUDIOSHIM_H
#define AUDIOSHIM_H

#include <e32std.h>
#include "hostlog.h"

void AudioShim_Init(THostLog* aLog);
TInt AudioShimReg_Init();

// Exit sequence, called by the UI thread. BeginStop: from now on the game gets silent facades only (its stop
// sequence must not open a real stream). Shutdown: no more old code is entered through audio callbacks and
// WriteL is ignored, so the game stops feeding the stream; call it before the game threads are killed.
void AudioShim_BeginStop();
void AudioShim_Shutdown();

#endif
