#include <e32base.h>
#include <mda/common/audio.h>            // TMdaAudioDataSettings
#include <mdaaudiooutputstream.h>        // real v3 CMdaAudioOutputStream
#include "audioshim.h"
#include "oldrt.h"
#include "shimreg.h"
#include "schedshim.h"

static THostLog* gLog = NULL;
static TInt gAudLogs = 0;
static TInt gStopping = 0;          // the app is closing: the game's stop sequence gets silent facades only
static TInt gPlayCompleteLogs = 0;
static TInt gShutdown = 0;          // set by the UI thread: no more old code is entered, no more writes
static TInt gRealCopied = 0;        // buffers the real stream has copied (statistics)
static TInt gFallbacks = 0;         // times a stream fell back to the silent fake
static TInt gOpensRequested = 0;
static TInt gOpenCompletes = 0;
const TInt KOpenWatchUs = 500000;    // real Open must complete within this, else retry / sink (max wait = (retries+1) x this)
const TInt KOpenRetries = 1;            // real Open retries before the sink thread takes over

// Old (v2) layouts, verified from the S60v2 mdaaudiooutputstream.h. GCC 2.9 vtable: two header
// words, then the virtuals in declaration order (CBase::~CBase first):
//   CMdaAudioOutputStream: [2] dtor  [3] SetAudioPropertiesL  [4] Open  [5] MaxVolume  [6] Volume
//                          [7] SetVolume  [8] SetPriority  [9] WriteL  [10] Stop  [11] Position
//   MMdaAudioOutputStreamCallback (no virtual dtor): [2] MaoscOpenComplete
//                          [3] MaoscBufferCopied(TInt, const TDesC8&)  [4] MaoscPlayComplete(TInt)
static TUint32 gAudVt[16];

const TInt KMaxBufs = 8;
const TInt KMaxVolume = 10;
// Share of the DEVICE maximum volume that the game's maximum maps to. The old game calls
// SetVolume(MaxVolume()) for "full"; on a v3 phone that is much louder than it was on the v2 device.
// Lower this if it is still too loud, raise it if it is too quiet.
const TInt KOutputPercent = 50;

enum TAudEvent { EvNone = 0, EvOpen, EvBuf, EvUnderflow, EvCancel };

class CAudTimer;
class CRealAudio;

struct TFakeStream
    {
    TUint32 iVptr;                  // must stay the first word: it is all the old code looks at
    TAny* iObserver;                // old MMdaAudioOutputStreamCallback* (subobject pointer)
    TUint64 iThread;                // creating (= only allowed) thread
    CAudTimer* iTimer;
    TInt iRate;
    TInt iChannels;
    TInt iVolume;
    TInt iPlaying;                  // at least one buffer written and not yet underflowed
    TInt iOpenPending;
    TInt iCancelPending;            // deliver MaoscPlayComplete(KErrCancel) after a Stop of a playing stream
    TInt iEvent;                    // what the timer delivers when it fires
    TInt iCount;                    // queued buffers
    const TDesC8* iBuf[KMaxBufs];
    TInt64 iPlayedMicros;
    TUint32 iPos[2];                // old TTimeIntervalMicroSeconds (iLow, iHigh), returned by Position()
    TInt iInRunL;
    TInt iZombie;                   // deleted by old code from inside a callback
    // Real playback. The old object stays the facade; a real v3 stream does the playing.
    CRealAudio* iReal;              // NULL = silent fake only
    TInt iUseReal;                  // 1 while the real stream drives the callbacks
    TInt iOpenDone;                 // real stream open and primed (properties set, queue flushed)
    TInt iPropsFromGame;            // the game called SetAudioPropertiesL (rate/channels are its own)
    TInt iPropsApplied;             // the real stream has been given rate/channels
	TInt iInCb;                     // inside a real-stream callback (old code may delete the stream)
	CPeriodic* iWatch;              // open watchdog (retries a real Open that never completes)
    TInt iRetries;
    TInt iUseSink;                  // real open failed: the sink thread plays the data
    TInt64 iCredit;                 // sink mode: elapsed time not yet turned into delivered buffers (us)
    TInt64 iLastUs;                 // sink mode: time of the previous EvBuf callback (0 = none)
    };

static TUint64 MyId()
    {
    RThread me;
    return me.Id().Id();
    }

static TBool Mine(TFakeStream* aS, const char* aWhat)
    {
    if (aS->iThread == MyId())
        {
        return ETrue;
        }
    if (gLog && gAudLogs < 40)
        {
        gAudLogs++;
        TPtrC8 w((const TUint8*)aWhat);
        gLog->Logf(_L8("audio: %S called from another thread, ignored"), &w);
        }
    return EFalse;
    }

static void ALog(const char* aText)             // lifecycle events only: always logged
    {
    if (gLog)
        {
        TPtrC8 p((const TUint8*)aText);
        gLog->Text(p);
        }
    }

// ---------------------------------------------------------------------------
static TInt BufMicros(TFakeStream* aS, const TDesC8* aBuf)
    {
    const TInt bytesPerSec = aS->iRate * aS->iChannels * 2;     // PCM16
    TInt64 us = ((TInt64)aBuf->Length() * 1000000) / (TInt64)(bytesPerSec > 0 ? bytesPerSec : 16000);
    if (us < 2000)
        {
        us = 2000;
        }
    if (us > 2000000)
        {
        us = 2000000;
        }
    return (TInt)us;
    }

static void Schedule(TFakeStream* aS);

// Reverse of CapsToRate (TMdaAudioDataSettings sample-rate caps: the same values in v2 and v3).
static TInt RateToCaps(TInt aHz)
    {
    switch (aHz)
        {
        case 8000:  return 0x00000010;
        case 11025: return 0x00000040;
        case 12000: return 0x00000080;
        case 16000: return 0x00000100;
        case 22050: return 0x00000400;
        case 24000: return 0x00000800;
        case 32000: return 0x00001000;
        case 44100: return 0x00004000;
        case 48000: return 0x00010000;
        case 96000: return 0x00020000;
        default:    return 0;
        }
    }

// The real v3 stream. Every function of it that can leave is TRAPped: a v3 leave is a C++
// exception and must never travel through old frames. It lives in the game thread (created
// from ShimAudioNewL there), so its callbacks run in the game thread's scheduler.
class CRealAudio : public CBase, public MMdaAudioOutputStreamCallback
    {
public:
    static CRealAudio* New(TFakeStream* aS)
        {
        CRealAudio* r = new CRealAudio;
        if (!r)
            {
            return NULL;
            }
        r->iS = aS;
        TInt err = KErrNone;
        TRAP(err, r->iStream = CMdaAudioOutputStream::NewL(*r));
        if (err != KErrNone || !r->iStream)
            {
            if (gLog)
                {
                gLog->Logf(_L8("audio: real CMdaAudioOutputStream::NewL failed (%d), silent fake only"), err);
                }
            delete r;
            return NULL;
            }
		r->iStream->SetPriority(EMdaPriorityNormal, EMdaPriorityPreferenceTimeAndQuality);
        return r;
        }
    ~CRealAudio()
        {
        delete iStream;
        }
    void MaoscOpenComplete(TInt aError);
    void MaoscBufferCopied(TInt aError, const TDesC8& aBuffer);
    void MaoscPlayComplete(TInt aError);

    TFakeStream* iS;
    CMdaAudioOutputStream* iStream;
    TMdaAudioDataSettings iSettings;
    };

class CAudTimer : public CTimer
    {
public:
    static CAudTimer* New(TFakeStream* aS)
        {
        CAudTimer* t = new CAudTimer(aS);
        if (!t)
            {
            return NULL;
            }
        TRAPD(err, t->ConstructL());
        if (err != KErrNone)
            {
            delete t;
            return NULL;
            }
        // CTimer::ConstructL creates the RTimer but does not add the object to the scheduler
        // (After() then panics with E32USER-CBase 51). Add it unless it is already added.
        if (!t->IsAdded())
            {
            CActiveScheduler::Add(t);
            }
        return t;
        }
    void RunL();
private:
    CAudTimer(TFakeStream* aS) : CTimer(EPriorityStandard), iS(aS) {}
    TFakeStream* iS;
    };

// Calls one of the three old callbacks (observer vtable word 2, 3 or 4).
static void Deliver(TFakeStream* aS, TInt aWord, TInt aErr, const TDesC8* aBuf, const char* aName)
    {
    TUint32* obs = (TUint32*)aS->iObserver;
    if (gShutdown || !obs || !obs[0])
        {
        return;
        }
    TUint32* vt = (TUint32*)obs[0];
    TInt res = 0;
    const TInt reason = TOldRt::CallArgs((TAny*)vt[aWord], (TInt)obs, aErr, (TInt)aBuf, 0, res);
    if (gLog && reason != KErrNone)
        {
        TPtrC8 n((const TUint8*)aName);
        gLog->Logf(_L8("audio: %S(%d): old code left with %d"), &n, aErr, reason);
        }
    }

// Deletes everything the shim owns for a stream; the caller decides about the stream memory.
static void FreeStream(TFakeStream* aS)
    {
    if (gLog)
        {
        gLog->Logf(_L8("audio: facade %08x freed (real stream %d)"), (TUint32)aS, aS->iReal != NULL);
        }
	delete aS->iWatch;
    aS->iWatch = NULL;
	delete aS->iReal;
    aS->iReal = NULL;
    delete aS->iTimer;
    aS->iTimer = NULL;
    }

static TInt DestroyCb(TAny* aS)
    {
    TFakeStream* s = (TFakeStream*)aS;
    FreeStream(s);
    User::Free(s);
    return 0;
    }

// The real stream must not be deleted from inside its own callback: do it from the scheduler.
// The small CAsyncCallBack object is intentionally leaked (one per stream deleted this way).
static void DeferDestroy(TFakeStream* aS)
    {
    CAsyncCallBack* cb = new CAsyncCallBack(TCallBack(DestroyCb, aS), CActive::EPriorityStandard);
    if (cb)
        {
        cb->CallBack();
        }
    }

// Real playback failed: continue silently with the fake timing so the game keeps running.
static void GoFake(TFakeStream* aS, const char* aWhy, TInt aErr)
    {
    if (!aS->iUseReal)
        {
        return;
        }
    aS->iUseReal = EFalse;
    gFallbacks++;
    if (gLog)
        {
        TPtrC8 w((const TUint8*)aWhy);
        gLog->Logf(_L8("audio: REAL STREAM -> silent fake: %S (%d)"), &w, aErr);
        }
    if (aS->iReal && aS->iOpenDone)
        {
        aS->iReal->iStream->Stop();
        }
    aS->iOpenDone = EFalse;
    Schedule(aS);
    }

// Real stream volume for the game's 0..KMaxVolume value, scaled by KOutputPercent.
static TInt RealVolume(TFakeStream* aS, TInt aMax)
    {
    return (TInt)(((TInt64)aS->iVolume * aMax * KOutputPercent) / (KMaxVolume * 100));
    }

// Gives the real stream the rate and channels the game asked for. v3 wants this after the open has
// completed and before the first buffer is written. The game does it inside its own
// MaoscOpenComplete, i.e. AFTER the real stream was opened, so the real open must not fix a rate of
// its own: opening at the 8000 Hz default and then playing the game's 16000 Hz data would run at half
// speed.
static TInt ApplyRealProps(TFakeStream* aS)
    {
    const TInt caps = RateToCaps(aS->iRate);
    if (!caps)
        {
        return KErrNotSupported;
        }
    TInt err = KErrNone;
    TRAP(err, aS->iReal->iStream->SetAudioPropertiesL(caps, aS->iChannels == 2 ? 0x04000000 : 0x02000000));
    if (gLog)
        {
        gLog->Logf(_L8("audio: real stream properties %d Hz, %d ch -> %d"), aS->iRate, aS->iChannels, err);
        }
    if (err == KErrNone)
        {
        aS->iPropsApplied = ETrue;
        }
    return err;
    }

void CRealAudio::MaoscOpenComplete(TInt aError)
    {
	gOpenCompletes++;
	if (iS->iWatch) { iS->iWatch->Cancel(); }
    if (gLog)
        {
		gLog->Logf(_L8("audio: real MaoscOpenComplete(%d), useReal=%d shutdown=%d queued=%d tick=%u"),
		   aError, iS->iUseReal, gShutdown, iS->iCount, User::TickCount());
        }
    TFakeStream* s = iS;
    if (!s->iUseReal || gShutdown)
        {
        return;
        }
    if (aError != KErrNone)
        {
        GoFake(s, "open failed", aError);
        return;
        }
    TInt err = KErrNone;
    // Rate and channels: apply now only if the game already gave them or has queued buffers;
    // otherwise wait for its SetAudioPropertiesL (or the first WriteL, which applies the defaults).
    if (s->iPropsFromGame || s->iCount > 0)
        {
        err = ApplyRealProps(s);
        if (err != KErrNone)
            {
            GoFake(s, "SetAudioPropertiesL failed", err);
            return;
            }
        }
    const TInt mv = iStream->MaxVolume();
    if (mv > 0)
        {
        iStream->SetVolume(RealVolume(s, mv));
        }
    s->iOpenDone = ETrue;
    // buffers the game wrote before the stream was open
    TInt flushed = 0;
    for (TInt i = 0; i < s->iCount && s->iUseReal; i++)
        {
        TRAP(err, iStream->WriteL(*s->iBuf[i]));
        if (err != KErrNone)
            {
            GoFake(s, "WriteL failed", err);
            break;
            }
        flushed++;
        }
    if (!s->iUseReal)
        {
        return;                     // the fake delivers MaoscOpenComplete
        }
    s->iOpenPending = EFalse;
    if (gLog)
        {
        gLog->Logf(_L8("audio: REAL stream open: %d Hz, %d ch, volume %d/%d, %d buffers flushed"),
                   s->iRate, s->iChannels, s->iVolume, KMaxVolume, flushed);
        }
    s->iInCb = ETrue;
    Deliver(s, 2, KErrNone, NULL, "MaoscOpenComplete");
    s->iInCb = EFalse;
    if (s->iZombie)
        {
        DeferDestroy(s);
        }
    }

void CRealAudio::MaoscBufferCopied(TInt aError, const TDesC8& aBuffer)
    {
    TFakeStream* s = iS;
    if (!s->iUseReal || gShutdown)
        {
        return;
        }
    if (aError == KErrAbort)
        {
        return;                     // a buffer dropped by Stop(): the old fake never reported those
        }
    if (aError != KErrNone)
        {
        GoFake(s, "buffer error", aError);
        return;
        }
    if (s->iCount == 0)
        {
        return;
        }
    TInt idx = -1;
    for (TInt i = 0; i < s->iCount; i++)
        {
        if (s->iBuf[i] == &aBuffer)
            {
            idx = i;
            break;
            }
        }
    if (idx < 0)
        {
        if (gLog && gAudLogs < 60)
            {
            gAudLogs++;
            gLog->Text(_L8("audio: real BufferCopied for an unknown buffer, taking the queue head"));
            }
        idx = 0;
        }
    const TDesC8* b = s->iBuf[idx];
    for (TInt i = idx + 1; i < s->iCount; i++)
        {
        s->iBuf[i - 1] = s->iBuf[i];
        }
    s->iCount--;
    s->iPlayedMicros += BufMicros(s, b);
    s->iPos[0] = (TUint32)(TUint64)s->iPlayedMicros;
    s->iPos[1] = (TUint32)((TUint64)s->iPlayedMicros >> 32);
    gRealCopied++;
    s->iInCb = ETrue;
    Deliver(s, 3, KErrNone, b, "MaoscBufferCopied");
    s->iInCb = EFalse;
    if (s->iZombie)
        {
        DeferDestroy(s);
        }
    }

void CRealAudio::MaoscPlayComplete(TInt aError)
    {
    TFakeStream* s = iS;
    if (!s->iUseReal || gShutdown)
        {
        return;
        }
    // The old API reported KErrCancel for a Stop(); the real one may report KErrNone.
    const TInt err = (aError == KErrNone) ? KErrCancel : aError;

    if (gLog && gPlayCompleteLogs < 20)
        {
        gPlayCompleteLogs++;
        gLog->Logf(_L8("audio: real MaoscPlayComplete(%d) -> old code gets %d"), aError, err);
        }
    s->iPlaying = EFalse;
    s->iInCb = ETrue;
    Deliver(s, 4, err, NULL, "MaoscPlayComplete");
    s->iInCb = EFalse;
    if (s->iZombie)
        {
        DeferDestroy(s);
        }
    }

void CAudTimer::RunL()
    {
    TFakeStream* s = iS;
    s->iInRunL = ETrue;
    const TInt ev = s->iEvent;
    s->iEvent = EvNone;
    switch (ev)
        {
        case EvOpen:
            s->iOpenPending = EFalse;
            Deliver(s, 2, KErrNone, NULL, "MaoscOpenComplete");
            break;
		case EvBuf:
            {
            TInt loops = 0;
            TBool first = ETrue;
            if (s->iUseSink)
                {
                TTime tn;
                tn.UniversalTime();
                const TInt64 now = tn.Int64();
                s->iCredit = (s->iLastUs == 0) ? 0 : (s->iCredit + (now - s->iLastUs));
                if (s->iCredit > 250000)
                    {
                    s->iCredit = 250000;
                    }
                s->iLastUs = now;
                }
            while (s->iCount > 0 && !s->iZombie)
                {
                const TDesC8* b = s->iBuf[0];
                const TInt dur = BufMicros(s, b);
                if (s->iUseSink && !first && (s->iCredit < dur || loops >= 4))
                    {
                    break;
                    }
                for (TInt i = 1; i < s->iCount; i++)
                    {
                    s->iBuf[i - 1] = s->iBuf[i];
                    }
                s->iCount--;
                s->iPlayedMicros += dur;
                s->iPos[0] = (TUint32)(TUint64)s->iPlayedMicros;
                s->iPos[1] = (TUint32)((TUint64)s->iPlayedMicros >> 32);
                if (s->iUseSink)
                    {
                    s->iCredit -= dur;
                    if (s->iCredit < 0)
                        {
                        s->iCredit = 0;
                        }
                    }
                first = EFalse;
                loops++;
                Deliver(s, 3, KErrNone, b, "MaoscBufferCopied");
                if (!s->iUseSink)
                    {
                    break;              // normal fake: exactly one buffer per timer tick
                    }
                }
            }
            break;
        case EvUnderflow:
            s->iPlaying = EFalse;
            Deliver(s, 4, KErrUnderflow, NULL, "MaoscPlayComplete");
            break;
        case EvCancel:
            Deliver(s, 4, KErrCancel, NULL, "MaoscPlayComplete");
            break;
        default:
            break;
        }
    s->iInRunL = EFalse;
    if (s->iZombie)                 // old code deleted the stream inside the callback
        {
        CRealAudio* r = s->iReal;
        s->iReal = NULL;
		delete s->iWatch;
        s->iWatch = NULL;
        User::Free(s);
        delete r;
        delete this;
        return;
        }
    Schedule(s);
    }

static void Schedule(TFakeStream* aS)
    {
    if (aS->iUseReal || !aS->iTimer || aS->iTimer->IsActive())
        {
        return;                         // real mode: the real stream drives the callbacks
        }
    TInt delay = 0;
    if (aS->iCancelPending)
        {
        aS->iCancelPending = EFalse;
        aS->iEvent = EvCancel;
        delay = 1000;
        }
    else if (aS->iOpenPending)
        {
        aS->iEvent = EvOpen;
        delay = 1000;
        }
    else if (aS->iCount > 0)
        {
        aS->iEvent = EvBuf;
        delay = BufMicros(aS, aS->iBuf[0]);
		if (aS->iUseSink)
            {
            delay -= (TInt)aS->iCredit;
            if (delay < 1000)
                {
                delay = 1000;
                }
            }
        }
    else if (aS->iPlaying)
        {
        aS->iEvent = EvUnderflow;
        delay = 1000;
        }
    else
        {
        return;
        }
    aS->iTimer->After(TTimeIntervalMicroSeconds32(delay));
    }

// ---------------------------------------------------------------------------
// the fake object's virtual functions (this = old object, AAPCS; arguments as in the v2 header)
// ---------------------------------------------------------------------------
static TInt CapsToRate(TInt aV)
    {
    switch (aV)
        {
        case 0x00000010: return 8000;
        case 0x00000040: return 11025;
        case 0x00000080: return 12000;
        case 0x00000100: return 16000;
        case 0x00000400: return 22050;
        case 0x00000800: return 24000;
        case 0x00001000: return 32000;
        case 0x00004000: return 44100;
        case 0x00010000: return 48000;
        case 0x00020000: return 96000;
        default: break;
        }
    return (aV >= 4000 && aV <= 96000) ? aV : 0;    // some games pass plain Hz
    }

static void VSetAudioPropertiesL(TFakeStream* s, TInt aRate, TInt aCh)      // [3]
    {
	if (gLog && gAudLogs < 40)
        {
        gAudLogs++;
        gLog->Logf(_L8("audio: game SetAudioPropertiesL rate=%x ch=%x (open done %d)"), aRate, aCh, s->iOpenDone);
        }
    const TInt hz = CapsToRate(aRate);
    if (hz)
        {
        s->iRate = hz;
        }
    s->iChannels = (aCh == 0x04000000 || aCh == 2) ? 2 : 1;
    s->iPropsFromGame = ETrue;

    // Real stream already open: pass the game's rate and channels on now (before its first WriteL).
    if (s->iUseReal && s->iOpenDone)
        {
        const TInt err = ApplyRealProps(s);
        if (err != KErrNone && !s->iPropsApplied)
            {
            GoFake(s, "SetAudioPropertiesL failed", err);
            }
        }
    }

// ---------------------------------------------------------------------------
// Sink thread: plays the audio of games whose real stream never opens in the UI thread.
// ---------------------------------------------------------------------------
const TInt KSinkSlots = 16;
const TInt KSinkSlotSize = 2048;        // bigger writes are split over several slots
const TInt KSinkMaxOut = 3;             // buffers handed to the real stream at once
enum { ESlotFree = 0, ESlotQueued, ESlotWriting };

// Upper bound for the queued (not yet handed over) slots: 16 slots minus KSinkMaxOut in flight is 13.
// The real limit is time based, about 0.2 s at the current format (see SinkPut).
const TInt KSinkMaxQueued = 12;

struct TSinkSlot
    {
    TInt iState;
    TUint32 iSeq;
    TInt iLen;
    TInt iRate;
    TInt iCh;
    TInt iVol;
    TUint8 iData[KSinkSlotSize];
    };

static TSinkSlot gSinkSlots[KSinkSlots];
static RMutex gSinkMx;
static TInt gSinkState = 0;             // 0 none, 1 running, 2 failed
static TInt gSinkQuit = 0;
static TInt gSinkFlush = 0;
static TUint32 gSinkSeq = 0;
static TThreadId gSinkId;
static TInt gSinkWritten = 0;
static TInt gSinkDropped = 0;

// Game thread: copy the data into free slots. Dropping the oldest queued data is better than
// blocking the game or growing without limit.
static void SinkPut(const TUint8* aP, TInt aLen, TInt aRate, TInt aCh, TInt aVol)
    {
    if (gSinkState != 1 || gSinkQuit)
        {
        return;
        }
    // At most about 0.2 s of audio waits in the queue, whatever the format (2 .. KSinkMaxQueued slots).
    TInt maxQueued = (aRate * aCh * 2 / 5) / KSinkSlotSize + 1;
    if (maxQueued < 2)
        {
        maxQueued = 2;
        }
    if (maxQueued > KSinkMaxQueued)
        {
        maxQueued = KSinkMaxQueued;
        }
    gSinkMx.Wait();
    while (aLen > 0)
        {
        const TInt n = aLen > KSinkSlotSize ? KSinkSlotSize : aLen;
        TInt idx = -1;
        TInt queued = 0;
        for (TInt i = 0; i < KSinkSlots; i++)
            {
            if (gSinkSlots[i].iState == ESlotQueued)
                {
                queued++;
                }
            }
        if (queued < maxQueued)
            {
            for (TInt i = 0; i < KSinkSlots; i++)
                {
                if (gSinkSlots[i].iState == ESlotFree)
                    {
                    idx = i;
                    break;
                    }
                }
            }
        if (idx < 0)
            {
			// queue full: the oldest queued slot is overwritten
            for (TInt i = 0; i < KSinkSlots; i++)
                {
                if (gSinkSlots[i].iState == ESlotQueued &&
                    (idx < 0 || (TInt32)(gSinkSlots[i].iSeq - gSinkSlots[idx].iSeq) < 0))
                    {
                    idx = i;
                    }
                }
            gSinkDropped++;
            }
        if (idx < 0)
            {
            break;                      // everything is being played: drop the new data
            }
        TSinkSlot& sl = gSinkSlots[idx];
        Mem::Copy(sl.iData, aP, n);
        sl.iLen = n;
        sl.iRate = aRate;
        sl.iCh = aCh;
        sl.iVol = aVol;
        sl.iSeq = ++gSinkSeq;
        sl.iState = ESlotQueued;
        aP += n;
        aLen -= n;
        }
    gSinkMx.Signal();
    }

static void SinkFlush()
    {
    if (gSinkState == 1)
        {
        gSinkFlush = 1;
        }
    }

class CSinkAudio : public CBase, public MMdaAudioOutputStreamCallback
    {
public:
    ~CSinkAudio()
        {
        delete iPoll;
        delete iStream;
        }
    void ConstructL();
    void MaoscOpenComplete(TInt aErr);
    void MaoscBufferCopied(TInt aErr, const TDesC8& aBuf);
    void MaoscPlayComplete(TInt aErr);
    void Poll();
    void Fail(const char* aWhy, TInt aErr);

    CMdaAudioOutputStream* iStream;
    CPeriodic* iPoll;
    TMdaAudioDataSettings iSettings;
    TPtrC8 iPtrs[KSinkSlots];           // must stay valid until MaoscBufferCopied
    TInt iCurRate;
    TInt iCurCh;
    TInt iCurVol;
    TInt iOut;
    TBool iOpen;
    };

static TInt SinkPollCb(TAny* aA)
    {
    ((CSinkAudio*)aA)->Poll();
    return 0;
    }

void CSinkAudio::ConstructL()
    {
    iCurVol = -1;
    iPoll = CPeriodic::NewL(CActive::EPriorityStandard);
    iStream = CMdaAudioOutputStream::NewL(*this);
    iStream->SetPriority(EMdaPriorityNormal, EMdaPriorityPreferenceTimeAndQuality);
    iSettings.iSampleRate = TMdaAudioDataSettings::ESampleRate8000Hz;
    iSettings.iChannels = TMdaAudioDataSettings::EChannelsMono;
    iSettings.iVolume = 0;
    iStream->Open(&iSettings);
    iPoll->Start(20000, 20000, TCallBack(SinkPollCb, this));
    }

void CSinkAudio::Fail(const char* aWhy, TInt aErr)
    {
    gSinkState = 2;
    iOpen = EFalse;
    if (gLog)
        {
        TPtrC8 w((const TUint8*)aWhy);
        gLog->Logf(_L8("sink: FAILED %S (%d)"), &w, aErr);
        }
    }

void CSinkAudio::MaoscOpenComplete(TInt aErr)
    {
    if (gLog)
        {
        gLog->Logf(_L8("sink: OpenComplete(%d) tick=%u"), aErr, User::TickCount());
        }
    if (aErr != KErrNone)
        {
        Fail("open", aErr);
        return;
        }
    iOpen = ETrue;
    }

void CSinkAudio::MaoscBufferCopied(TInt aErr, const TDesC8& aBuf)
    {
	if (aErr == KErrAbort)
	{
		return;                         // dropped by Stop(); the flush already freed the slots
	}
    for (TInt i = 0; i < KSinkSlots; i++)
        {
        if (&aBuf == &iPtrs[i])
            {
            if (gSinkSlots[i].iState == ESlotWriting)
                {
                gSinkSlots[i].iState = ESlotFree;
                if (iOut > 0)
                    {
                    iOut--;
                    }
                }
            return;
            }
        }
    }

void CSinkAudio::MaoscPlayComplete(TInt)
    {
    }                                   // underflow is normal: the next WriteL restarts playback

void CSinkAudio::Poll()
    {
    if (gSinkQuit)
        {
        CActiveScheduler::Stop();
        return;
        }
    if (!iOpen)
        {
        return;
        }
    if (gSinkFlush)
        {
        gSinkFlush = 0;
        gSinkMx.Wait();
        for (TInt i = 0; i < KSinkSlots; i++)
            {
            gSinkSlots[i].iState = ESlotFree;
            }
        gSinkMx.Signal();
        if (iOut > 0)
            {
            iStream->Stop();
            }
        iOut = 0;
        return;
        }
    while (iOut < KSinkMaxOut)
        {
        TInt idx = -1;
        gSinkMx.Wait();
        for (TInt i = 0; i < KSinkSlots; i++)
            {
            if (gSinkSlots[i].iState == ESlotQueued &&
                (idx < 0 || (TInt32)(gSinkSlots[i].iSeq - gSinkSlots[idx].iSeq) < 0))
                {
                idx = i;
                }
            }
        if (idx >= 0)
            {
            TSinkSlot& c = gSinkSlots[idx];
            // rate or channels change only when nothing is being played
            if ((c.iRate != iCurRate || c.iCh != iCurCh) && iOut > 0)
                {
                idx = -1;
                }
            else
                {
                c.iState = ESlotWriting;
                }
            }
        gSinkMx.Signal();
        if (idx < 0)
            {
            break;
            }
        TSinkSlot& sl = gSinkSlots[idx];
        TInt err = KErrNone;
        if (sl.iRate != iCurRate || sl.iCh != iCurCh)
            {
            const TInt caps = RateToCaps(sl.iRate);
            if (!caps)
                {
                err = KErrNotSupported;
                }
            else
                {
                TRAP(err, iStream->SetAudioPropertiesL(caps, sl.iCh == 2 ? 0x04000000 : 0x02000000));
                }
            if (gLog)
                {
                gLog->Logf(_L8("sink: properties %d Hz, %d ch -> %d"), sl.iRate, sl.iCh, err);
                }
            if (err == KErrNone)
                {
                iCurRate = sl.iRate;
                iCurCh = sl.iCh;
                }
            }
        if (err == KErrNone && sl.iVol != iCurVol)
            {
            const TInt mv = iStream->MaxVolume();
            if (mv > 0)
                {
                iStream->SetVolume((TInt)(((TInt64)sl.iVol * mv * KOutputPercent) / (KMaxVolume * 100)));
                }
            iCurVol = sl.iVol;
            }
        if (err == KErrNone)
            {
            iPtrs[idx].Set(sl.iData, sl.iLen);
            TRAP(err, iStream->WriteL(iPtrs[idx]));
            }
        if (err != KErrNone)
            {
            sl.iState = ESlotFree;
            Fail("write", err);
            return;
            }
        iOut++;
        if (gSinkWritten++ == 0 && gLog)
            {
            gLog->Logf(_L8("sink: first write tick=%u"), User::TickCount());
            }
        }
    }

static TInt SinkThread(TAny*)
    {
    CTrapCleanup* tc = CTrapCleanup::New();
    CActiveScheduler* sch = new CActiveScheduler;
    if (!tc || !sch)
        {
        gSinkState = 2;
        return KErrNoMemory;
        }
    CActiveScheduler::Install(sch);
    CSinkAudio* a = new CSinkAudio;
    TInt err = KErrNoMemory;
    if (a)
        {
        TRAP(err, a->ConstructL());
        }
    if (err == KErrNone)
        {
        CActiveScheduler::Start();
        }
    else
        {
        gSinkState = 2;
        if (gLog)
            {
            gLog->Logf(_L8("sink: setup failed (%d)"), err);
            }
        }
    delete a;
    delete sch;
    delete tc;
    return KErrNone;
    }

static void SinkStart()
    {
    if (gSinkState != 0)
        {
        return;
        }
    if (gSinkMx.CreateLocal() != KErrNone)
        {
        gSinkState = 2;
        return;
        }
    gSinkState = 1;
    RThread t;
    if (t.Create(_L("audsink"), SinkThread, 0x4000, 0x4000, 0x100000, NULL) != KErrNone)
        {
        gSinkState = 2;
        return;
        }
    gSinkId = t.Id();
    t.Resume();
    t.Close();
    if (gLog)
        {
        gLog->Logf(_L8("sink: thread started tick=%u"), User::TickCount());
        }
    }

// UI thread, at shutdown: stop the sink thread and wait for it briefly.
static void SinkShutdown()
    {
    if (gSinkState == 0)
        {
        return;
        }
    gSinkQuit = 1;
    RThread t;
    if (t.Open(gSinkId) == KErrNone)
        {
        for (TInt i = 0; i < 50 && t.ExitType() == EExitPending; i++)
            {
            User::After(20000);
            }
        if (t.ExitType() == EExitPending)
            {
            t.Kill(0);
            }
        t.Close();
        }
    if (gLog)
        {
        gLog->Logf(_L8("sink: shutdown, buffers written=%d, dropped=%d"), gSinkWritten, gSinkDropped);
        }
	}

// Open watchdog. Some phones (N86, E71 with Atomanic) never answer the real Open of a stream that
// lives in the UI thread. After KOpenRetries retries the game carries on with the fake callbacks
// while the sink thread plays the audio through a real stream of its own.
static TInt OpenWatchCb(TAny* aS)
    {
    TFakeStream* s = (TFakeStream*)aS;
    s->iWatch->Cancel();
    if (!s->iReal || !s->iUseReal || s->iOpenDone || s->iZombie)
        {
        return 0;
        }
    if (s->iRetries >= KOpenRetries)
        {
        s->iUseSink = ETrue;
        SinkStart();
		delete s->iReal->iStream;
        s->iReal->iStream = NULL;
        GoFake(s, "real open never completed, using the sink thread", s->iRetries);
        return 0;
        }
    s->iRetries++;
    if (gLog)
        {
        gLog->Logf(_L8("audio: no OpenComplete after %d ms, retry %d"), KOpenWatchUs / 1000, s->iRetries);
        }
    CRealAudio* r = s->iReal;
    delete r->iStream;
    r->iStream = NULL;
    TInt err = KErrNone;
    TRAP(err, r->iStream = CMdaAudioOutputStream::NewL(*r));
    if (err != KErrNone || !r->iStream)
        {
        GoFake(s, "retry NewL failed", err);
        return 0;
        }
    r->iStream->SetPriority(EMdaPriorityNormal, EMdaPriorityPreferenceTimeAndQuality);
    r->iStream->Open(&r->iSettings);
    s->iWatch->Start(KOpenWatchUs, KOpenWatchUs, TCallBack(OpenWatchCb, s));
    return 0;
    }

static void VOpen(TFakeStream* s, TAny* /*aSettings*/)                       // [4]
    {
    if (!Mine(s, "Open"))
        {
        return;
        }
    ALog("audio: Open");
    gOpensRequested++;
    if (gLog)
        {
        gLog->Logf(_L8("audio: Open: real stream %d, game props %d (%d Hz, %d ch)"),
                   s->iUseReal, s->iPropsFromGame, s->iRate, s->iChannels);
        }
    s->iOpenPending = ETrue;

    if (s->iUseReal)
        {
        // The real stream is opened at 8000 Hz mono; the game's own rate and channels are applied
        // after the open completes (ApplyRealProps).
        TMdaAudioDataSettings& st = s->iReal->iSettings;
        st.iSampleRate = TMdaAudioDataSettings::ESampleRate8000Hz;
        st.iChannels = TMdaAudioDataSettings::EChannelsMono;
        st.iVolume = 0;

        s->iReal->iStream->Open(&s->iReal->iSettings);      // MaoscOpenComplete comes from the real stream
		if (!s->iWatch)
            {
            TRAPD(werr, s->iWatch = CPeriodic::NewL(CActive::EPriorityStandard));
            (void)werr;
            }
        if (s->iWatch)
            {
			s->iWatch->Start(KOpenWatchUs, KOpenWatchUs, TCallBack(OpenWatchCb, s));
            }
        }
    else
        {
        Schedule(s);
        }
    }

static TInt VMaxVolume(TFakeStream*) { return KMaxVolume; }                  // [5]
static TInt VVolume(TFakeStream* s) { return s->iVolume; }                   // [6]
static void VSetVolume(TFakeStream* s, TInt aV)                              // [7]
    {
    if (aV < 0)
        {
        aV = 0;
        }
    if (aV > KMaxVolume)
        {
        aV = KMaxVolume;
        }
    s->iVolume = aV;
    if (s->iUseReal && s->iOpenDone)
        {
        const TInt mv = s->iReal->iStream->MaxVolume();
        if (mv > 0)
            {
            s->iReal->iStream->SetVolume(RealVolume(s, mv));
            }
        }
    }

// static void VSetPriority(TFakeStream*, TInt, TInt) {}                        // [8]

static void VSetPriority(TFakeStream* s, TInt aPri, TInt aPref)               // [8]
    {
    if (!Mine(s, "SetPriority"))
        {
        return;
        }
    if (s->iUseReal && s->iReal && s->iReal->iStream)
        {
        s->iReal->iStream->SetPriority(aPri, (TMdaPriorityPreference)aPref);
        if (gLog)
            {
            gLog->Logf(_L8("audio: game SetPriority(%d, %d) forwarded"), aPri, aPref);
            }
        }
    }

static void VWriteL(TFakeStream* s, const TDesC8* aData)                     // [9]
    {
    if (!Mine(s, "WriteL"))
        {
        return;
        }

	    if (gLog && gAudLogs < 40)
        {
        gAudLogs++;
        gLog->Logf(_L8("audio: game WriteL len=%d (open done %d, real %d)"), aData->Length(), s->iOpenDone, s->iUseReal);
        }
    if (s->iCount >= KMaxBufs)
        {
        ALog("audio: WriteL queue full, leaving KErrOverflow");
        TOldRt::Leave(KErrOverflow);
        }
    if (gShutdown)
        {
        return;                         // the app is closing: let the game stop feeding audio
        }
    s->iBuf[s->iCount++] = aData;
    s->iPlaying = ETrue;

    if (s->iUseReal)
        {
        if (s->iOpenDone)               // before that the open-complete handler flushes the queue
            {
            TInt err = KErrNone;
            if (!s->iPropsApplied)          // the game never set properties: apply the defaults now
                {
                err = ApplyRealProps(s);
                }
            if (err == KErrNone)
                {
                TRAP(err, s->iReal->iStream->WriteL(*aData));
                }
            if (err != KErrNone)
                {
                GoFake(s, "WriteL failed", err);
                }
            }
        }
	else
        {
        if (s->iUseSink)
            {
				if (s->iCount == 1)
                {
                s->iLastUs = 0;         // queue was empty: restart the pacing clock
                }
            SinkPut(aData->Ptr(), aData->Length(), s->iRate, s->iChannels, s->iVolume);
            }
        Schedule(s);
        }
    }

static void VStop(TFakeStream* s)                                            // [10]
    {
    if (!Mine(s, "Stop"))
        {
        return;
        }
    ALog("audio: Stop");
    if (s->iUseReal)
        {
        // The real stream reports the end through MaoscPlayComplete; dropped buffers come back
        // as KErrAbort and are ignored (see CRealAudio::MaoscBufferCopied).
        if (s->iOpenDone)
            {
            s->iReal->iStream->Stop();
            }
        s->iCount = 0;
        s->iPlaying = EFalse;
        return;
        }

	if (s->iUseSink)
        {
		s->iLastUs = 0;
        s->iCredit = 0;
        SinkFlush();
        }

    const TBool wasPlaying = s->iPlaying;
    if (s->iTimer)
        {
        s->iTimer->Cancel();
        }
    s->iCount = 0;
    s->iPlaying = EFalse;
    s->iEvent = EvNone;
    if (wasPlaying)
        {
        s->iCancelPending = ETrue;
        Schedule(s);                    // inside RunL the timer is idle, so arming here is allowed
        }
    }

static const TAny* VPosition(TFakeStream* s) { return s->iPos; }             // [11]

static void VDtor(TFakeStream* s, TInt aFlags)                               // [2]
    {
    ALog("audio: stream deleted by old code");
    s->iObserver = NULL;                // no callback may reach old code from now on
    if (s->iTimer)
        {
        s->iTimer->Cancel();
        }
    if (s->iReal && s->iOpenDone)
        {
        s->iReal->iStream->Stop();
        }
    if (s->iInRunL || s->iInCb)
        {
        s->iZombie = ETrue;             // freed from the scheduler when the callback has returned
        return;
        }
    FreeStream(s);
    if (aFlags & 1)
        {
        User::Free(s);
        }
    }

// euser-style factory: static CMdaAudioOutputStream* NewL(MMdaAudioOutputStreamCallback&, CMdaServer*)
static TAny* ShimAudioNewL(TAny* aCallback, TAny* /*aServer*/)
    {
    if (!CActiveScheduler::Current())
        {
        ALog("audio: NewL without an active scheduler, leaving KErrNotReady");
        TOldRt::Leave(KErrNotReady);
        }
    TFakeStream* s = (TFakeStream*)User::AllocZ(sizeof(TFakeStream));
    if (!s)
        {
        TOldRt::Leave(KErrNoMemory);
        }
    s->iVptr = (TUint32)gAudVt;
    s->iObserver = aCallback;
    s->iThread = MyId();
    s->iRate = 8000;
    s->iChannels = 1;
    s->iVolume = KMaxVolume / 2;
    s->iTimer = CAudTimer::New(s);
    if (!s->iTimer)
        {
        User::Free(s);
        TOldRt::Leave(KErrNoMemory);
        }

    s->iReal = gStopping ? NULL : CRealAudio::New(s);       // no real stream while the app is closing
    s->iUseReal = (s->iReal != NULL);
    if (gLog)
        {
        gLog->Logf(_L8("audio: CMdaAudioOutputStream::NewL(observer %08x) -> facade %08x, real stream %d"),
                   (TUint32)aCallback, (TUint32)s, s->iUseReal);
        }
    return s;
    }

// ---------------------------------------------------------------------------
void AudioShim_Init(THostLog* aLog)
    {
    gLog = aLog;
    gAudVt[2]  = (TUint32)&VDtor;
    gAudVt[3]  = (TUint32)&VSetAudioPropertiesL;
    gAudVt[4]  = (TUint32)&VOpen;
    gAudVt[5]  = (TUint32)&VMaxVolume;
    gAudVt[6]  = (TUint32)&VVolume;
    gAudVt[7]  = (TUint32)&VSetVolume;
    gAudVt[8]  = (TUint32)&VSetPriority;
    gAudVt[9]  = (TUint32)&VWriteL;
    gAudVt[10] = (TUint32)&VStop;
    gAudVt[11] = (TUint32)&VPosition;
    }

static const TShimRegEntry KAudioShimEntries[] =
    {
    { "mediaclientaudiostream", 1, (TUint32)&ShimAudioNewL, 0, "audioshim", NULL }
    };

TInt AudioShimReg_Init()
    {
    return ShimReg_Add(KAudioShimEntries, sizeof(KAudioShimEntries) / sizeof(KAudioShimEntries[0]));
    }

void AudioShim_BeginStop()
    {
    gStopping = 1;
    }

// Called by the UI thread before the game threads are killed: no old code is entered through the
// audio callbacks any more and WriteL is ignored, so the game stops feeding the stream.
void AudioShim_Shutdown()
    {
    gShutdown = 1;
    if (gLog)
        {
		gLog->Logf(_L8("audio: shutdown, opens requested=%d, real open completions=%d, real buffers copied=%d, fallbacks to fake=%d"),
                   gOpensRequested, gOpenCompletes, gRealCopied, gFallbacks);
		}
	    SinkShutdown();
    }
