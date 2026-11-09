// Compiler runtime helpers and a few euser odds and ends of the old game.
//
// The old binary was built with GCC 2.9 / soft float in FPA word order: a double travels in two
// registers as (r0 = HIGH word, r1 = LOW word) -- verified with the literal 131070.0 at 0x180a8
// (words 40ffffe0, 00000000). EABI soft-float wants (r0 = low, r1 = high), so these shims swap.
// 64-bit integers (__divdi3) use the same layout in both: low word in the lower register.
#ifndef RTSHIM_H
#define RTSHIM_H

#include <e32std.h>

TInt RtShim_Init();                            // explicitly add the immutable rtshim table

#endif
