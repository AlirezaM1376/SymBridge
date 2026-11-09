// Constructors of the old framework base classes (eikcore / avkon) that old code calls when it
// constructs its application, document and AppUi objects. They need no host objects: the old object
// was allocated zeroed (CBase new), so most of them only have to hand back "this".
// The layouts used here (CEikDocument 36 bytes, CEikApplication iProcess at +20) were verified against the
// Captain Dynamo binary (see the notes at each function).
// The table is added to the shim registry explicitly (EikShim_Init), owner "eikshim".
#include <e32std.h>
#include "shimreg.h"
#include "eikshim.h"

static TAny* ShimNoopCtor(TAny* aThis)              // base ctor placeholder: memory is already zeroed
    {
    return aThis;
    }

// eikcore 214: CEikApplication::~CEikApplication() called as the BASE destructor from the game's own
// destructor. The base part is a plain zeroed block (see the constructor shim), so there is nothing to
// release; the game's destructor frees the object itself.
static void ShimBaseDtorNoop(TAny* /*aThis*/, TInt /*aFlags*/)
    {
    }

static TAny* ShimAppUiCtor(TAny* aThis)             // CEikAppUi::CEikAppUi(): the members stay zero
    {
    return aThis;
    }

// Old CEikDocument, 36 bytes (verified: the game allocated 36):
//  0 vptr | 4 iContainer | 8 iApplication | 12 iApaProcess | 16 iSpare
//  20 iAppUi | 24 iEditStore | 28 iChanged | 32 iAppFileMode
// Old CEikApplication: iProcess is at offset 20 (total 556, verified).
static TAny* ShimAknDocCtor(TUint8* aThis, TUint8* aApp)   // CAknDocument::CAknDocument(CEikApplication&), avkon 2053
    {
    *(TUint8**)(aThis + 8) = aApp;                      // iApplication
    *(TAny**)(aThis + 12)  = *(TAny**)(aApp + 20);      // iApaProcess
    return aThis;
    }

static const TShimRegEntry KEikShimEntries[] =
    {
    { "eikcore", 214,  (TUint32)&ShimBaseDtorNoop, 0, "eikshim", NULL },   // CEikApplication::~CEikApplication()
    { "eikcore", 222,  (TUint32)&ShimNoopCtor,   0, "eikshim", NULL },   // CEikApplication::CEikApplication()
    { "eikcore", 224,  (TUint32)&ShimAppUiCtor,  0, "eikshim", NULL },   // CEikAppUi::CEikAppUi()
    { "avkon",   2053, (TUint32)&ShimAknDocCtor, 0, "eikshim", NULL },    // CAknDocument::CAknDocument(CEikApplication&)
    { "eikcore", 219,  (TUint32)&ShimAknDocCtor, 0, "eikshim", NULL }   // CEikDocument::CEikDocument(CEikApplication&)
    };

TInt EikShim_Init()
    {
    return ShimReg_Add(KEikShimEntries, sizeof(KEikShimEntries) / sizeof(KEikShimEntries[0]));
    }
