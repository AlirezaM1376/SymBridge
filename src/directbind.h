// The single list of imports bound directly to the real v3 function (see directbind.cpp for the rules).
#ifndef DIRECTBIND_H
#define DIRECTBIND_H

#include <e32std.h>

TInt DirectBind_Init();     // explicitly add the immutable DIRECT table to the shim registry

#endif
