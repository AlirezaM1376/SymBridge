#ifndef HOSTSHIM_E32STD_H
#define HOSTSHIM_E32STD_H
#include <string.h>
#include <ctype.h>
#include "e32def.h"
class TDesC8
    {
public:
    TDesC8(const TText8* aZ) : iPtr(aZ), iLen((TInt)strlen((const char*)aZ)) {}
    TInt CompareF(const TDesC8& aOther) const        // case-folded compare, like the real one
        {
        TInt n = iLen < aOther.iLen ? iLen : aOther.iLen;
        for (TInt i = 0; i < n; i++)
            {
            int a = tolower(iPtr[i]), b = tolower(aOther.iPtr[i]);
            if (a != b) return a - b;
            }
        return iLen - aOther.iLen;
        }
private:
    const TText8* iPtr;
    TInt iLen;
    };
class TPtrC8 : public TDesC8
    {
public:
    TPtrC8(const TText8* aZ) : TDesC8(aZ) {}
    };
#ifndef NULL
#define NULL 0
#endif
#endif
