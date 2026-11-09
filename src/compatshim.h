// Self-contained euser / hal / plpvariant shims (see compatshim.cpp for what is in and the rule for it).
#ifndef COMPATSHIM_H
#define COMPATSHIM_H

#include <e32std.h>

TInt CompatShim_Init();                                 // explicitly add the immutable compatshim table to the registry

#endif
