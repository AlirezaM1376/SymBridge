// Opt-in real-device check that imported direct mappings resolve to exports and that
// three known euser functions can be called through ordinals from the generated map.
#ifndef IMPORTTEST_H
#define IMPORTTEST_H

#include "hostlog.h"
#include "legacyimage.h"

void RunImportMapTest(THostLog& aLog, const CLegacyImage& aImg);

#endif
