#include "hostlog.h"

const TInt KLineMax = 512;              // longer lines are cut
const TUint32 KFlushIntervalMs = 250;

// Truncate instead of panicking when a line is too long.
class TLogTruncate : public TDes8Overflow
    {
public:
    void Overflow(TDes8& /*aDes*/) {}
    };

TInt THostLog::Open(RFs& /*aFs*/, const TDesC& aFileName)
    {
    TInt err = iFs.Connect();
    if (err != KErrNone)
        {
        return err;
        }
    iFs.ShareAuto();                    // the game thread logs through this session too
    err = iLock.CreateLocal();
    if (err == KErrNone)
        {
        err = iFile.Replace(iFs, aFileName, EFileWrite | EFileShareAny);
        if (err != KErrNone)
            {
            iLock.Close();
            }
        }
    if (err != KErrNone)
        {
        iFs.Close();
        return err;
        }
    iLastFlush = User::NTickCount();
    iOpen = ETrue;
    return KErrNone;
    }

void THostLog::Close()
    {
    if (iOpen)
        {
        iOpen = EFalse;
        iLock.Wait();
        iFile.Flush();
        iFile.Close();
        iLock.Signal();
        iLock.Close();
        iFs.Close();
        }
    }

void THostLog::Text(const TDesC8& aText)
    {
    if (!Enabled())
        {
        return;
        }
    TBuf8<KLineMax + 2> line;           // text and line end go out in one write
    line.Copy(aText.Left(KLineMax));
    line.Append(_L8("\r\n"));
    iLock.Wait();
    iFile.Write(line);
    const TUint32 now = User::NTickCount();
	if (iFlushEach || now - iLastFlush >= KFlushIntervalMs)
        {
        iFile.Flush();
        iLastFlush = now;
        }
    iLock.Signal();
    }

void THostLog::Logf(TRefByValue<const TDesC8> aFmt, ...)
    {
    if (!Enabled())
        {
        return;                         // nothing is formatted when logging is off
        }
    VA_LIST list;
    VA_START(list, aFmt);
    TBuf8<KLineMax> buf;
    TLogTruncate trunc;
    buf.AppendFormatList(aFmt, list, &trunc);
    VA_END(list);
    Text(buf);
    }
