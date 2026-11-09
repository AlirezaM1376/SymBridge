// Memory / cleanup / leave shims of the old game's euser imports (see memshim.cpp).
#ifndef MEMSHIM_H
#define MEMSHIM_H

#include <e32std.h>

TInt MemShim_Init();                                // explicitly add the immutable memshim table to the registry

#endif
